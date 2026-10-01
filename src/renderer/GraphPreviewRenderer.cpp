#include "renderer/GraphPreviewRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <DirectXPackedVector.h>

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/PipelineState.h"
#include "graphics/RootSignature.h"
#include "graphics/Texture.h"
#include "renderer/IBLBaker.h"
#include "renderer/Mesh.h"
#include "resource/ResourceManager.h"
#include "resource/ShaderRuntimeCompiler.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace dx12e
{
namespace
{

// ---------------------------------------------------------------------------------------------
// b1（PerFrameConstants, 1536 バイト）。Lighting.hlsli / Application の FrameConstants / ModelThumbnailRenderer と同じレイアウト
// ---------------------------------------------------------------------------------------------
struct FrameConstants
{
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
    XMFLOAT3   lightDir;   float time;
    XMFLOAT3   lightColor; float ambientStrength;
    XMFLOAT4X4 cascadeViewProj[4];
    XMFLOAT4   cascadeSplitsView;
    XMFLOAT4   shadowParams;
    XMFLOAT3   cameraPos;  float aoEnabled;
    u32        numPointLights; u32 numSpotLights;
    float      spotShadowTexel; float pointShadowNear;
    XMFLOAT4   clusterParams;
    XMFLOAT4   clusterGrid;
    XMFLOAT4   clusterViewport;
    XMFLOAT4   clusterExtra;
    XMFLOAT4   pcssParams;
    XMFLOAT4   ddgiOrigin;
    XMFLOAT4   ddgiSpacing;
    XMFLOAT4   ddgiCounts;
    XMFLOAT4   _clusterReserved[38];
    XMFLOAT4   giParams;
    XMFLOAT4   giParams2;
    XMFLOAT4X4 spotShadowMatrix[4];
    float      iblIntensity;
    float      maxPrefilterMip;
    u32        hasIBL;
    float      skyboxIntensity;
    float      contactShadowEnabled;
    XMFLOAT3   normalFilterParams;
};
static_assert(sizeof(FrameConstants) == 1536, "FrameConstants must be 1536 bytes (Lighting.hlsli PerFrameConstants)");

constexpr u32 kCbSlotBytes = 1536;   // 256 の倍数
constexpr u32 kCbSlotsPerFrame = 2;  // 0 = メイン / 1 = タイル・RAW
constexpr u32 kClusterTable = 9;     // kSlotClusterSRV のテーブル長: t13-15 + t18-21 + t22-23

struct Immediate   // 環境のベイク用の専用キュー（メインのキューとは独立。完了まで待つ）
{
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    bool Init(ID3D12Device* dev)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return ev != nullptr;
    }
    void SubmitAndWait()
    {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        q->ExecuteCommandLists(1, l);
        ++fv;
        q->Signal(fence.Get(), fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
    }
    ~Immediate() { if (ev) CloseHandle(ev); }
};

D3D12_RESOURCE_BARRIER Trans(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

// ---------------------------------------------------------------------------------------------
// 環境（手続き生成のキューブ）。D3D の面の向き: +X,-X,+Y,-Y,+Z,-Z。u = 左→右, v = 上→下（-1..1）
// ---------------------------------------------------------------------------------------------
XMFLOAT3 CubeDir(int face, float u, float v)
{
    XMFLOAT3 d;
    switch (face)
    {
    case 0: d = { 1.0f, -v, -u }; break;
    case 1: d = { -1.0f, -v, u }; break;
    case 2: d = { u, 1.0f, v }; break;
    case 3: d = { u, -1.0f, -v }; break;
    case 4: d = { u, -v, 1.0f }; break;
    default: d = { -u, -v, -1.0f }; break;
    }
    const float n = 1.0f / std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return {d.x * n, d.y * n, d.z * n};
}

float Sm(float a, float b, float x)
{
    const float t = std::min(1.0f, std::max(0.0f, (x - a) / (b - a)));
    return t * t * (3.0f - 2.0f * t);
}
float Dot3(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XMFLOAT3 Norm3(float x, float y, float z)
{
    const float n = 1.0f / std::sqrt(x * x + y * y + z * z);
    return {x * n, y * n, z * n};
}

// 面光源（ソフトボックス）: 中心方向 c の周りの角度 r0（内側・全強度）〜 r1（外側・0）
float Blob(const XMFLOAT3& d, const XMFLOAT3& c, float r0deg, float r1deg)
{
    const float cd = Dot3(d, c);
    return Sm(std::cos(r1deg * 0.0174532925f), std::cos(r0deg * 0.0174532925f), cd);
}

XMFLOAT3 EnvRadiance(int env, const XMFLOAT3& d)
{
    float r = 0, g = 0, b = 0;
    auto add = [&](float k, float cr, float cg, float cb) { r += k * cr; g += k * cg; b += k * cb; };
    if (env == 0)   // スタジオ: 暗めの無彩色の部屋 + ソフトボックス（キー・フィル・リム・天井）
    {
        const float up = std::max(0.0f, d.y), dn = std::max(0.0f, -d.y);
        add(0.05f + 0.10f * up, 0.95f, 0.97f, 1.0f);    // 天井 / 壁のうっすらした明るさ
        add(0.03f * dn, 1.0f, 0.97f, 0.92f);
        add(7.0f * Blob(d, Norm3(-0.55f, 0.62f, -0.56f), 16.0f, 34.0f), 1.0f, 0.97f, 0.92f);   // キー（左上手前・暖）
        add(2.2f * Blob(d, Norm3(0.75f, 0.10f, -0.65f), 12.0f, 30.0f), 0.85f, 0.92f, 1.0f);    // フィル（右手前・寒）
        add(5.0f * Blob(d, Norm3(0.05f, 0.45f, 0.89f), 10.0f, 24.0f), 1.0f, 1.0f, 1.0f);       // リム（奥）
        add(1.6f * Blob(d, Norm3(0.0f, 1.0f, 0.0f), 22.0f, 46.0f), 1.0f, 1.0f, 1.0f);          // 天井
    }
    else if (env == 1)   // 屋外: 空の勾配 + 太陽 + 地面
    {
        const float y = d.y;
        const float t = std::pow(std::min(1.0f, std::max(0.0f, y)), 0.55f);
        const float sr = 0.78f + (0.20f - 0.78f) * t, sg = 0.86f + (0.42f - 0.86f) * t, sb = 1.0f + (0.90f - 1.0f) * t;
        const float sky = Sm(-0.05f, 0.05f, y);
        r += sky * sr; g += sky * sg; b += sky * sb;
        const float gr = 1.0f - sky;
        r += gr * 0.20f; g += gr * 0.18f; b += gr * 0.15f;
        const XMFLOAT3 sun = Norm3(-0.42f, 0.55f, -0.72f);
        const float sd = Dot3(d, sun);
        add(40.0f * Sm(0.9985f, 0.9994f, sd), 1.0f, 0.93f, 0.80f);
        add(1.2f * std::pow(std::max(0.0f, sd), 48.0f), 1.0f, 0.90f, 0.75f);
    }
    else   // 暗所: ほぼ黒 + 寒色のリムと暖色の小さな光
    {
        add(0.006f, 0.8f, 0.9f, 1.0f);
        add(4.5f * Blob(d, Norm3(-0.75f, 0.35f, 0.55f), 8.0f, 22.0f), 0.55f, 0.75f, 1.0f);
        add(3.0f * Blob(d, Norm3(0.70f, 0.20f, -0.65f), 6.0f, 16.0f), 1.0f, 0.62f, 0.35f);
    }
    return {r, g, b};
}

// キューブ 1 枚（RGBA16F・mip 全段）を CPU で作って data[subresource]（= mip + face * mips）へ入れる
void BuildEnvCube(int env, u32 size, u32 mips, std::vector<std::vector<u16>>& data)
{
    data.assign(static_cast<size_t>(mips) * 6, {});
    std::vector<std::vector<float>> f32(static_cast<size_t>(mips) * 6);
    for (int face = 0; face < 6; ++face)
    {
        auto& m0 = f32[static_cast<size_t>(face) * mips];
        m0.resize(static_cast<size_t>(size) * size * 4);
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x)
            {
                const float u = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
                const float v = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
                const XMFLOAT3 c = EnvRadiance(env, CubeDir(face, u, v));
                float* p = &m0[(static_cast<size_t>(y) * size + x) * 4];
                p[0] = c.x; p[1] = c.y; p[2] = c.z; p[3] = 1.0f;
            }
        for (u32 m = 1; m < mips; ++m)
        {
            const u32 s = std::max(1u, size >> m), sp = std::max(1u, size >> (m - 1));
            const auto& src = f32[static_cast<size_t>(face) * mips + m - 1];
            auto& dst = f32[static_cast<size_t>(face) * mips + m];
            dst.resize(static_cast<size_t>(s) * s * 4);
            for (u32 y = 0; y < s; ++y)
                for (u32 x = 0; x < s; ++x)
                    for (int k = 0; k < 4; ++k)
                    {
                        float a = 0;
                        for (u32 dy = 0; dy < 2; ++dy)
                            for (u32 dx = 0; dx < 2; ++dx)
                                a += src[(static_cast<size_t>(std::min(sp - 1, y * 2 + dy)) * sp + std::min(sp - 1, x * 2 + dx)) * 4 + static_cast<size_t>(k)];
                        dst[(static_cast<size_t>(y) * s + x) * 4 + static_cast<size_t>(k)] = a * 0.25f;
                    }
        }
    }
    for (size_t i = 0; i < f32.size(); ++i)
    {
        data[i].resize(f32[i].size());
        for (size_t k = 0; k < f32[i].size(); ++k)
            data[i][k] = PackedVector::XMConvertFloatToHalf(std::min(f32[i][k], 60000.0f));
    }
}

// ---------------------------------------------------------------------------------------------
// 表示（トーンマップ）パス
// ---------------------------------------------------------------------------------------------
const char* kTonemapHlsl = R"HLSL(
Texture2D<float4> gColor : register(t0);
Texture2D<float>  gDepth : register(t1);
cbuffer C : register(b0)
{
    float4 topColor;
    float4 botColor;
    float4 params;     // x = exposure, y = vignette, z = bgGrid（0 = 無し）, w = pixels
    float4 reserved;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}
float3 ACESFilm(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}
float4 PSMain(VSOut i) : SV_TARGET
{
    int2 px = int2(i.pos.xy);
    float d = gDepth.Load(int3(px, 0));
    float2 c2 = i.uv * 2.0 - 1.0;
    float3 bg = lerp(topColor.rgb, botColor.rgb, saturate(i.uv.y));
    bg *= 1.0 - params.y * dot(c2, c2) * 0.5;
    if (d >= 1.0) return float4(bg, 1.0);
    float3 hdr = gColor.Load(int3(px, 0)).rgb * params.x;
    float3 c = pow(ACESFilm(hdr), 1.0 / 2.2);
    return float4(c, 1.0);
}
)HLSL";

} // namespace

// ===============================================================================================
struct GraphPreviewRenderer::Impl
{
    InitDesc d;
    ID3D12Device* dev = nullptr;
    ID3D12Device5* dev5 = nullptr;

    ComPtr<ID3D12Resource> depth, atlas;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvColor{}, rtvDisplay{}, rtvAtlas{}, rtvRaw{}, dsv{};
    u32 srvColorDepth = DescriptorHeap::kInvalidIndex;   // 連続 2 個: [0] = 色 / [1] = 深度
    u32 srvDisplay = DescriptorHeap::kInvalidIndex, srvAtlas = DescriptorHeap::kInvalidIndex;
    bool atlasCleared = false;

    ComPtr<ID3D12Resource> cb;
    u8* cbMapped = nullptr;

    std::unique_ptr<Mesh> sphere, plane, cylinder, cube, quad;

    // ダミー（メイン RS のテーブルは何かを指している必要がある。実際には読まれない）
    ComPtr<ID3D12Resource> aoWhite, ssBlack, dummyBuf;
    u32 aoWhiteSrv = DescriptorHeap::kInvalidIndex, ssBlackSrv = DescriptorHeap::kInvalidIndex;
    u32 clusterSrv = DescriptorHeap::kInvalidIndex;   // 9 個連続（t13-15 / t18-21 / t22-23。RootSignature.cpp の kSlotClusterSRV）

    ComPtr<ID3D12RootSignature> tmRS;
    ComPtr<ID3D12PipelineState> tmPso;

    struct Env
    {
        ComPtr<ID3D12Resource> cube;
        std::unique_ptr<IBLBaker> baker;
        float topColor[3] = {0, 0, 0}, botColor[3] = {0, 0, 0};
        float exposure = 1.0f, vignette = 0.3f;
        float lightColor[3] = {1, 1, 1};
        float lightIntensity = 1.0f;
        float iblIntensity = 1.0f;
    };
    Env envs[3];
    bool envReady = false, envFailed = false;
    std::unique_ptr<GraphPreviewRenderer::FlatLighting> flat;

    // ------------------------------------------------------------------ 初期化
    bool CreateResources(GraphPreviewRenderer& self);
    bool CreateMeshes();
    bool CreateDummies();
    bool CreateTonemap();
    bool BakeEnvironments();

    void FillFrame(FrameConstants& fc, const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cam, const XMFLOAT3& lightTravel,
                   const XMFLOAT3& lightColor, float ambient, bool ibl, const Env* env, float time);
    void BindCommon(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS cbAddr, const Env* env);
    D3D12_GPU_VIRTUAL_ADDRESS CbAddr(u32 frame, u32 slot) const
    {
        return cb->GetGPUVirtualAddress() + static_cast<UINT64>((frame % GraphMaterialSystem::kFrames) * kCbSlotsPerFrame + slot) * kCbSlotBytes;
    }
    u8* CbPtr(u32 frame, u32 slot) const
    {
        return cbMapped + static_cast<size_t>((frame % GraphMaterialSystem::kFrames) * kCbSlotsPerFrame + slot) * kCbSlotBytes;
    }
    void DrawMesh(ID3D12GraphicsCommandList* cmd, Mesh& m, const XMMATRIX& world, const XMMATRIX& viewProj, const GraphMaterialSystem::DrawParams& dp);
};

// ---------------------------------------------------------------------------------------------
bool GraphPreviewRenderer::Impl::CreateResources(GraphPreviewRenderer& self)
{
    auto tex2d = [&](u32 w, u32 h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Flags = flags;
        ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, cv, IID_PPV_ARGS(&out)));
    };
    const D3D12_RESOURCE_STATES kPsr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    try
    {
        D3D12_CLEAR_VALUE cvC{};
        cvC.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        tex2d(kMainSize, kMainSize, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kPsr, &cvC, self.m_color);
        D3D12_CLEAR_VALUE cvD{};
        cvD.Format = DXGI_FORMAT_D32_FLOAT;
        cvD.DepthStencil.Depth = 1.0f;
        tex2d(kMainSize, kMainSize, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, kPsr, &cvD, depth);
        D3D12_CLEAR_VALUE cvL{};
        cvL.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex2d(kMainSize, kMainSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kPsr, &cvL, self.m_display);
        tex2d(kAtlasSize, kAtlasSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kPsr, &cvL, atlas);
        D3D12_CLEAR_VALUE cvR{};
        cvR.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        tex2d(kTilePixels, kTilePixels, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kPsr, &cvR, self.m_rawTile);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;
        ThrowIfFailed(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        ThrowIfFailed(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)));
        const u32 rs = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvColor = h;   dev->CreateRenderTargetView(self.m_color.Get(), nullptr, rtvColor);
        h.ptr += rs; rtvDisplay = h; dev->CreateRenderTargetView(self.m_display.Get(), nullptr, rtvDisplay);
        h.ptr += rs; rtvAtlas = h;   dev->CreateRenderTargetView(atlas.Get(), nullptr, rtvAtlas);
        h.ptr += rs; rtvRaw = h;     dev->CreateRenderTargetView(self.m_rawTile.Get(), nullptr, rtvRaw);
        dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(depth.Get(), &dd, dsv);

        // SRV（共有のシェーダー可視ヒープ）
        srvColorDepth = d.srvHeap->AllocateBlock(2);
        srvDisplay = d.srvHeap->AllocateIndex();
        srvAtlas = d.srvHeap->AllocateIndex();
        if (srvColorDepth == DescriptorHeap::kInvalidIndex || srvDisplay == DescriptorHeap::kInvalidIndex || srvAtlas == DescriptorHeap::kInvalidIndex)
            throw std::runtime_error("SRV heap exhausted");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dev->CreateShaderResourceView(self.m_color.Get(), &sd, d.srvHeap->GetCpuHandle(srvColorDepth));
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        dev->CreateShaderResourceView(depth.Get(), &sd, d.srvHeap->GetCpuHandle(srvColorDepth + 1));
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dev->CreateShaderResourceView(self.m_display.Get(), &sd, d.srvHeap->GetCpuHandle(srvDisplay));
        dev->CreateShaderResourceView(atlas.Get(), &sd, d.srvHeap->GetCpuHandle(srvAtlas));

        // 定数バッファ（UPLOAD・常時マップ。フレーム 3 本 × 2 スロット）
        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = static_cast<UINT64>(kCbSlotBytes) * kCbSlotsPerFrame * GraphMaterialSystem::kFrames;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb)));
        void* m = nullptr;
        ThrowIfFailed(cb->Map(0, nullptr, &m));
        cbMapped = static_cast<u8*>(m);
        std::memset(cbMapped, 0, static_cast<size_t>(bd.Width));
    }
    catch (const std::exception& e)
    {
        Logger::Error("GraphPreviewRenderer: リソース作成に失敗: {}", e.what());
        return false;
    }
    return true;
}

bool GraphPreviewRenderer::Impl::CreateMeshes()
{
    try
    {
        GraphicsDevice& g = *d.device;
        const XMFLOAT4 white = {1, 1, 1, 1};
        auto V = [&](float px, float py, float pz, float nx, float ny, float nz, float u, float v, float tx, float ty, float tz, float tw) {
            Vertex vt{};
            vt.position = {px, py, pz};
            vt.normal = {nx, ny, nz};
            vt.color = white;
            vt.texCoord = {u, v};
            vt.tangent = {tx, ty, tz, tw};
            return vt;
        };
        // 球: 既存の生成器（U = theta 方向）。半径 1
        sphere = std::make_unique<Mesh>();
        sphere->InitializeAsSphere(g, 1.0f, 96, 48);

        // 平面: XY 平面・カメラ側（-Z）が表。2 x 2
        {
            std::vector<Vertex> vs;
            std::vector<u32> is;
            vs.push_back(V(-1, -1, 0, 0, 0, -1, 0, 1, 1, 0, 0, 1));
            vs.push_back(V(-1, 1, 0, 0, 0, -1, 0, 0, 1, 0, 0, 1));
            vs.push_back(V(1, 1, 0, 0, 0, -1, 1, 0, 1, 0, 0, 1));
            vs.push_back(V(1, -1, 0, 0, 0, -1, 1, 1, 1, 0, 0, 1));
            is = {0, 1, 2, 0, 2, 3};
            plane = std::make_unique<Mesh>();
            plane->Initialize(g, vs, is);
        }
        // ノード内サムネイル用の全画面の四角: 位置 = NDC そのまま（MVP は恒等）、UV (0,0) = 左上
        {
            std::vector<Vertex> vs;
            vs.push_back(V(-1, -1, 0, 0, 0, -1, 0, 1, 1, 0, 0, 1));
            vs.push_back(V(-1, 1, 0, 0, 0, -1, 0, 0, 1, 0, 0, 1));
            vs.push_back(V(1, 1, 0, 0, 0, -1, 1, 0, 1, 0, 0, 1));
            vs.push_back(V(1, -1, 0, 0, 0, -1, 1, 1, 1, 0, 0, 1));
            std::vector<u32> is = {0, 1, 2, 0, 2, 3};
            quad = std::make_unique<Mesh>();
            quad->Initialize(g, vs, is);
        }
        // 円柱: 半径 0.75・高さ 1.6（軸 = Y）。側面 UV = (角度, 高さ)・キャップは平面 UV
        {
            const int N = 96;
            const float R = 0.75f, H = 0.8f;
            std::vector<Vertex> vs;
            std::vector<u32> is;
            for (int i = 0; i <= N; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(N);
                const float a = t * 6.28318530718f;
                const float c = std::cos(a), s = std::sin(a);
                vs.push_back(V(R * c, H, R * s, c, 0, s, t, 0.0f, -s, 0, c, 1));
                vs.push_back(V(R * c, -H, R * s, c, 0, s, t, 1.0f, -s, 0, c, 1));
            }
            for (int i = 0; i < N; ++i)
            {
                const u32 a = static_cast<u32>(i * 2), b = a + 1, c = a + 2, d2 = a + 3;
                is.insert(is.end(), {a, c, b, b, c, d2});
            }
            for (int cap = 0; cap < 2; ++cap)
            {
                const float y = cap == 0 ? H : -H, ny = cap == 0 ? 1.0f : -1.0f;
                const u32 center = static_cast<u32>(vs.size());
                vs.push_back(V(0, y, 0, 0, ny, 0, 0.5f, 0.5f, 1, 0, 0, 1));
                for (int i = 0; i <= N; ++i)
                {
                    const float a = static_cast<float>(i) / static_cast<float>(N) * 6.28318530718f;
                    const float c = std::cos(a), s = std::sin(a);
                    vs.push_back(V(R * c, y, R * s, 0, ny, 0, 0.5f + 0.5f * c, 0.5f - 0.5f * s * ny, 1, 0, 0, 1));
                }
                for (int i = 0; i < N; ++i)
                {
                    const u32 a = center + 1 + static_cast<u32>(i), b = a + 1;
                    if (cap == 0) is.insert(is.end(), {center, b, a});
                    else is.insert(is.end(), {center, a, b});
                }
            }
            cylinder = std::make_unique<Mesh>();
            cylinder->Initialize(g, vs, is);
        }
        // 立方体: 半辺 0.8。面ごとに UV 0..1・接線
        {
            const float h = 0.8f;
            struct Face { float nx, ny, nz, tx, ty, tz; };
            const Face faces[6] = {
                {0, 0, -1, 1, 0, 0},   // 手前（カメラ側）
                {1, 0, 0, 0, 0, 1},
                {0, 0, 1, -1, 0, 0},
                {-1, 0, 0, 0, 0, -1},
                {0, 1, 0, 1, 0, 0},
                {0, -1, 0, 1, 0, 0},
            };
            std::vector<Vertex> vs;
            std::vector<u32> is;
            for (const Face& f : faces)
            {
                // 面内の基底: T（接線）と B = cross(N, T)（v が増える向き = 下）
                const float bx = f.ny * f.tz - f.nz * f.ty, by = f.nz * f.tx - f.nx * f.tz, bz = f.nx * f.ty - f.ny * f.tx;
                const u32 base = static_cast<u32>(vs.size());
                const float sx[4] = {-1, -1, 1, 1}, sy[4] = {-1, 1, 1, -1};   // 左下・左上・右上・右下（T 方向 = +x、-B 方向 = 上）
                for (int k = 0; k < 4; ++k)
                {
                    const float px = f.nx * h + f.tx * h * sx[k] - bx * h * sy[k];
                    const float py = f.ny * h + f.ty * h * sx[k] - by * h * sy[k];
                    const float pz = f.nz * h + f.tz * h * sx[k] - bz * h * sy[k];
                    vs.push_back(V(px, py, pz, f.nx, f.ny, f.nz, (sx[k] + 1) * 0.5f, (1 - sy[k]) * 0.5f, f.tx, f.ty, f.tz, 1));
                }
                is.insert(is.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            }
            cube = std::make_unique<Mesh>();
            cube->Initialize(g, vs, is);
        }
    }
    catch (const std::exception& e)
    {
        Logger::Error("GraphPreviewRenderer: メッシュ作成に失敗: {}", e.what());
        return false;
    }
    return true;
}

bool GraphPreviewRenderer::Impl::CreateDummies()
{
    try
    {
        // 1x1 の白（R8）と黒（RGBA16F）。中身は DEFAULT ヒープの初期値 0 のままだと黒なので、白は UPLOAD 経由でなく
        // 「UPLOAD ヒープのテクスチャ」を使わず、Immediate で書く（環境のベイクと同じ機会に）。ここでは作るだけ。
        auto mk = [&](DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = 1; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = fmt; rd.SampleDesc.Count = 1;
            ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out)));
        };
        mk(DXGI_FORMAT_R8_UNORM, aoWhite);
        mk(DXGI_FORMAT_R16G16B16A16_FLOAT, ssBlack);
        aoWhiteSrv = d.srvHeap->AllocateIndex();
        ssBlackSrv = d.srvHeap->AllocateIndex();
        clusterSrv = d.srvHeap->AllocateBlock(kClusterTable);
        if (aoWhiteSrv == DescriptorHeap::kInvalidIndex || ssBlackSrv == DescriptorHeap::kInvalidIndex || clusterSrv == DescriptorHeap::kInvalidIndex)
            throw std::runtime_error("SRV heap exhausted");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8_UNORM;
        dev->CreateShaderResourceView(aoWhite.Get(), &sd, d.srvHeap->GetCpuHandle(aoWhiteSrv));
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dev->CreateShaderResourceView(ssBlack.Get(), &sd, d.srvHeap->GetCpuHandle(ssBlackSrv));

        // クラスタライト / インデックス / カウント / デカール / DDGI（9 個）: 中身は読まれない。共通の小さなバッファを指す
        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&dummyBuf)));
        D3D12_SHADER_RESOURCE_VIEW_DESC bs{};
        bs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bs.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        bs.Format = DXGI_FORMAT_UNKNOWN;
        bs.Buffer.NumElements = 16;
        bs.Buffer.StructureByteStride = 16;
        for (u32 i = 0; i < kClusterTable; ++i) dev->CreateShaderResourceView(dummyBuf.Get(), &bs, d.srvHeap->GetCpuHandle(clusterSrv + i));
        // ダミーの初期化（白 / 黒）: UPLOAD 経由でコピー（専用の即時キューで完了まで待つ）
        Immediate exd;
        if (!exd.Init(dev)) throw std::runtime_error("即時キューを作れません");
        ID3D12GraphicsCommandList* cld = exd.list.Get();
        std::vector<ComPtr<ID3D12Resource>> keepd;
        auto uploadTex = [&](ID3D12Resource* dst, const void* pixel, u32 bytesPerPixel) {
            D3D12_HEAP_PROPERTIES up{};
            up.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> u;
            ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&u)));
            void* m = nullptr;
            ThrowIfFailed(u->Map(0, nullptr, &m));
            std::memset(m, 0, 256);
            std::memcpy(m, pixel, bytesPerPixel);
            u->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION s{}, t{};
            s.pResource = u.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint.Footprint.Format = dst->GetDesc().Format;
            s.PlacedFootprint.Footprint.Width = 1; s.PlacedFootprint.Footprint.Height = 1; s.PlacedFootprint.Footprint.Depth = 1;
            s.PlacedFootprint.Footprint.RowPitch = 256;
            t.pResource = dst;
            t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cld->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
            D3D12_RESOURCE_BARRIER b = Trans(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cld->ResourceBarrier(1, &b);
            keepd.push_back(u);
        };
        const u8 one = 255;
        const u16 zero4[4] = {0, 0, 0, 0};
        uploadTex(aoWhite.Get(), &one, 1);
        uploadTex(ssBlack.Get(), zero4, 8);

        exd.SubmitAndWait();
    }
    catch (const std::exception& e)
    {
        Logger::Error("GraphPreviewRenderer: ダミーの作成に失敗: {}", e.what());
        return false;
    }
    return true;
}

bool GraphPreviewRenderer::Impl::CreateTonemap()
{
    try
    {
        ShaderRuntimeCompiler comp;
        if (!comp.Initialize()) throw std::runtime_error("DXC を初期化できません");
        auto build = [&](const wchar_t* entry, const wchar_t* profile, std::vector<u8>& out) {
            ShaderRuntimeCompiler::CompileRequest req;
            req.hlslPath = L"graph_preview_tonemap.hlsl";
            req.sourceText = kTonemapHlsl;
            req.entry = entry;
            req.profile = profile;
            auto r = comp.Compile(req);
            if (!r.success) throw std::runtime_error(r.errorLog);
            out = std::move(r.dxil);
        };
        std::vector<u8> vs, ps;
        build(L"VSMain", L"vs_6_0", vs);
        build(L"PSMain", L"ps_6_0", ps);

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;
        params[1].Constants.Num32BitValues = 16;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) throw std::runtime_error("RS のシリアライズに失敗");
        ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&tmRS)));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = tmRS.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tmPso)));
    }
    catch (const std::exception& e)
    {
        Logger::Error("GraphPreviewRenderer: 表示パスの作成に失敗: {}", e.what());
        return false;
    }
    return true;
}

// 環境: 手続きキューブ（3 種）→ IBLBaker。専用の即時キューで完了まで待つ（初回の 1 回だけ）
bool GraphPreviewRenderer::Impl::BakeEnvironments()
{
    if (envReady) return true;
    if (envFailed) return false;
    try
    {
        Immediate ex;
        if (!ex.Init(dev)) throw std::runtime_error("即時キューを作れません");
        constexpr u32 kEnvSize = 128, kEnvMips = 8;
        ID3D12GraphicsCommandList* cl = ex.list.Get();

        // 環境キューブ
        std::vector<ComPtr<ID3D12Resource>> keep;
        for (int e = 0; e < 3; ++e)
        {
            std::vector<std::vector<u16>> data;
            BuildEnvCube(e, kEnvSize, kEnvMips, data);
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = kEnvSize; rd.Height = kEnvSize; rd.DepthOrArraySize = 6; rd.MipLevels = kEnvMips;
            rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc.Count = 1;
            ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&envs[e].cube)));
            const u32 nsub = kEnvMips * 6;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(nsub);
            std::vector<u32> rows(nsub);
            std::vector<u64> rowBytes(nsub);
            u64 total = 0;
            dev->GetCopyableFootprints(&rd, 0, nsub, 0, fp.data(), rows.data(), rowBytes.data(), &total);
            D3D12_HEAP_PROPERTIES up{};
            up.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> u;
            ThrowIfFailed(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&u)));
            u8* map = nullptr;
            ThrowIfFailed(u->Map(0, nullptr, reinterpret_cast<void**>(&map)));
            for (u32 face = 0; face < 6; ++face)
                for (u32 m = 0; m < kEnvMips; ++m)
                {
                    const u32 sub = m + face * kEnvMips;   // D3D12 のサブリソース番号 = mip + 配列 * mipCount
                    const auto& src = data[static_cast<size_t>(face) * kEnvMips + m];
                    const u32 w = std::max(1u, kEnvSize >> m);
                    for (u32 y = 0; y < rows[sub]; ++y)
                        std::memcpy(map + fp[sub].Offset + static_cast<size_t>(y) * fp[sub].Footprint.RowPitch, &src[static_cast<size_t>(y) * w * 4], static_cast<size_t>(w) * 8);
                }
            u->Unmap(0, nullptr);
            for (u32 sub = 0; sub < nsub; ++sub)
            {
                D3D12_TEXTURE_COPY_LOCATION s{}, t{};
                s.pResource = u.Get();
                s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                s.PlacedFootprint = fp[sub];
                t.pResource = envs[e].cube.Get();
                t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                t.SubresourceIndex = sub;
                cl->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
            }
            D3D12_RESOURCE_BARRIER b = Trans(envs[e].cube.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cl->ResourceBarrier(1, &b);
            keep.push_back(u);
        }
        ex.SubmitAndWait();

        // ベイク（環境ごとに 1 回。Bake は即時 Free の運用 = 完了待ち込み）
        for (int e = 0; e < 3; ++e)
        {
            envs[e].baker = std::make_unique<IBLBaker>();
            envs[e].baker->Initialize(*d.device, d.shaderDirW);
            envs[e].baker->Bake(*d.device, cl, *d.srvHeap, envs[e].cube.Get());
            ex.SubmitAndWait();
        }
        // 環境ごとの見た目（背景・露出・平行光）
        Env& st = envs[0];
        st.topColor[0] = 0.165f; st.topColor[1] = 0.175f; st.topColor[2] = 0.195f;
        st.botColor[0] = 0.045f; st.botColor[1] = 0.048f; st.botColor[2] = 0.055f;
        st.exposure = 1.0f; st.vignette = 0.45f;
        st.lightColor[0] = 1.0f; st.lightColor[1] = 0.96f; st.lightColor[2] = 0.92f; st.lightIntensity = 1.6f; st.iblIntensity = 1.0f;
        Env& od = envs[1];
        od.topColor[0] = 0.40f; od.topColor[1] = 0.56f; od.topColor[2] = 0.80f;
        od.botColor[0] = 0.74f; od.botColor[1] = 0.79f; od.botColor[2] = 0.84f;
        od.exposure = 0.75f; od.vignette = 0.25f;
        od.lightColor[0] = 1.0f; od.lightColor[1] = 0.94f; od.lightColor[2] = 0.84f; od.lightIntensity = 3.2f; od.iblIntensity = 1.0f;
        Env& dk = envs[2];
        dk.topColor[0] = 0.030f; dk.topColor[1] = 0.032f; dk.topColor[2] = 0.040f;
        dk.botColor[0] = 0.006f; dk.botColor[1] = 0.006f; dk.botColor[2] = 0.010f;
        dk.exposure = 1.3f; dk.vignette = 0.55f;
        dk.lightColor[0] = 0.75f; dk.lightColor[1] = 0.85f; dk.lightColor[2] = 1.0f; dk.lightIntensity = 0.5f; dk.iblIntensity = 1.0f;
        envReady = true;
    }
    catch (const std::exception& e)
    {
        Logger::Error("GraphPreviewRenderer: 環境のベイクに失敗: {}", e.what());
        envFailed = true;
        return false;
    }
    return true;
}

void GraphPreviewRenderer::Impl::FillFrame(FrameConstants& fc, const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cam,
                                           const XMFLOAT3& lightTravel, const XMFLOAT3& lightColor, float ambient, bool ibl, const Env* env, float time)
{
    std::memset(&fc, 0, sizeof(fc));
    XMStoreFloat4x4(&fc.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(proj));
    fc.lightDir = lightTravel;
    fc.time = time;
    fc.lightColor = lightColor;
    fc.ambientStrength = ambient;
    // 影なし: cascade0 = 恒等・分割を巨大値（CSM はコンパイル時に外れるが、フルの経路（比較用）でも無影になる値）
    for (int i = 0; i < 4; ++i) XMStoreFloat4x4(&fc.cascadeViewProj[i], XMMatrixIdentity());
    fc.cascadeSplitsView = {1e9f, 1e9f, 1e9f, 1e9f};
    fc.shadowParams = {1.0f / 4096.0f, 0.0f, 0.0f, 0.0f};
    fc.cameraPos = cam;
    fc.aoEnabled = 0.0f;
    fc.clusterGrid = {16.0f, 9.0f, 24.0f, 0.0f};    // .w = 0: クラスタード無効
    fc.clusterExtra = {0.0f, 128.0f, 0.0f, 0.0f};   // 総灯数 0・デカール 0
    fc.iblIntensity = ibl && env ? env->iblIntensity : 0.0f;
    fc.maxPrefilterMip = 4.0f;
    fc.hasIBL = ibl ? 1u : 0u;
    fc.skyboxIntensity = 0.0f;
    fc.contactShadowEnabled = 0.0f;
}

void GraphPreviewRenderer::Impl::BindCommon(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS cbAddr, const Env* env)
{
    DescriptorHeap& h = *d.srvHeap;
    // ★バインドレス RS: SetDescriptorHeaps を RS の設定より前に
    ID3D12DescriptorHeap* heaps[] = {h.GetHeap()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(d.rootSignature->Get());
    cmd->SetGraphicsRootConstantBufferView(RootSignature::kSlotPerFrame, cbAddr);
    Texture* white = d.resources->GetDefaultWhiteTexture();
    const D3D12_GPU_DESCRIPTOR_HANDLE whiteH = h.GetGpuHandle(white->GetSrvIndex());
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotSRVTable, whiteH);
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotShadowSRV, whiteH);
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotPunctualShadowSRV, whiteH);
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotAOSRV, h.GetGpuHandle(aoWhiteSrv));
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotContactShadowSRV, h.GetGpuHandle(aoWhiteSrv));
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotSsrSRV, h.GetGpuHandle(ssBlackSrv));
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotSsgiSRV, h.GetGpuHandle(ssBlackSrv));
    cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotClusterSRV, h.GetGpuHandle(clusterSrv));
    if (env && env->baker && env->baker->IsValid())
        cmd->SetGraphicsRootDescriptorTable(RootSignature::kSlotIBLTable, h.GetGpuHandle(env->baker->GetIrradianceSrv()));
}

void GraphPreviewRenderer::Impl::DrawMesh(ID3D12GraphicsCommandList* cmd, Mesh& m, const XMMATRIX& world, const XMMATRIX& viewProj,
                                          const GraphMaterialSystem::DrawParams& dp)
{
    // b0: MVP + Model + 自由枠 8 float（ObjectParameter 用。0）= 40 DWORD
    struct { XMFLOAT4X4 mvp; XMFLOAT4X4 mdl; float free8[8]; } obj{};
    XMStoreFloat4x4(&obj.mvp, XMMatrixTranspose(world * viewProj));
    XMStoreFloat4x4(&obj.mdl, XMMatrixTranspose(world));
    cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPerObject, 40, &obj, 0);
    // b2（グラフ材質の読み替え）: recordBase / poolSrvIndex / graphFlags / packedTint / uvScaleOffset / packedEmissive
    struct { u32 recordBase, poolSrv, flags, tint; float uv[4]; u32 emissive; } mat{};
    mat.recordBase = dp.recordBase;
    mat.poolSrv = dp.poolSrvIndex;
    mat.tint = 0xFFFFFFFFu;
    mat.uv[0] = 1.0f; mat.uv[1] = 1.0f;
    cmd->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 9, &mat, 0);
    cmd->SetPipelineState(dp.pso);
    const auto& vbv = m.GetVertexBuffer().GetView();
    const auto& ibv = m.GetIndexBuffer().GetView();
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(m.GetIndexCount(), 1, 0, 0, 0);
}

// ===============================================================================================
GraphPreviewRenderer::GraphPreviewRenderer() = default;
GraphPreviewRenderer::~GraphPreviewRenderer() { Shutdown(); }

bool GraphPreviewRenderer::Initialize(const InitDesc& d)
{
    Shutdown();
    if (!d.device || !d.rootSignature || !d.resources || !d.srvHeap || !d.graphs || !d.rootSignature->IsBindless()) return false;
    m_impl = std::make_unique<Impl>();
    Impl& I = *m_impl;
    I.d = d;
    if (!I.d.shaderDirW.empty() && I.d.shaderDirW.back() != L'/' && I.d.shaderDirW.back() != L'\\') I.d.shaderDirW.push_back(L'\\');
    I.dev5 = d.device->GetDevice();
    I.dev = I.dev5;
    if (!I.CreateResources(*this) || !I.CreateMeshes() || !I.CreateDummies() || !I.CreateTonemap())
    {
        Shutdown();
        return false;
    }
    m_valid = true;
    return true;
}

void GraphPreviewRenderer::Shutdown()
{
    if (m_impl)
    {
        Impl& I = *m_impl;
        if (I.d.srvHeap)
        {
            if (I.srvColorDepth != DescriptorHeap::kInvalidIndex) I.d.srvHeap->FreeBlock(I.srvColorDepth, 2);
            if (I.srvDisplay != DescriptorHeap::kInvalidIndex) I.d.srvHeap->Free(I.srvDisplay);
            if (I.srvAtlas != DescriptorHeap::kInvalidIndex) I.d.srvHeap->Free(I.srvAtlas);
            if (I.aoWhiteSrv != DescriptorHeap::kInvalidIndex) I.d.srvHeap->Free(I.aoWhiteSrv);
            if (I.ssBlackSrv != DescriptorHeap::kInvalidIndex) I.d.srvHeap->Free(I.ssBlackSrv);
            if (I.clusterSrv != DescriptorHeap::kInvalidIndex) I.d.srvHeap->FreeBlock(I.clusterSrv, kClusterTable);
            for (auto& e : I.envs)
                if (e.baker && e.baker->GetSrvBlockStart() != DescriptorHeap::kInvalidIndex)
                    I.d.srvHeap->FreeBlock(e.baker->GetSrvBlockStart(), e.baker->GetSrvBlockCount());
        }
        for (auto& e : I.envs)
            if (e.baker) e.baker->Reset();
        if (I.cb && I.cbMapped) I.cb->Unmap(0, nullptr);
        m_impl.reset();
    }
    m_color.Reset();
    m_display.Reset();
    m_rawTile.Reset();
    m_valid = false;
}

bool GraphPreviewRenderer::EnsureEnvironments()
{
    return m_impl && m_impl->BakeEnvironments();
}

void GraphPreviewRenderer::SetFlatLightingOverride(const FlatLighting* f)
{
    if (!m_impl) return;
    if (f) m_impl->flat = std::make_unique<FlatLighting>(*f);
    else m_impl->flat.reset();
}

u64 GraphPreviewRenderer::MainDisplayHandle() const
{
    return m_impl ? m_impl->d.srvHeap->GetGpuHandle(m_impl->srvDisplay).ptr : 0;
}

ID3D12Resource* GraphPreviewRenderer::AtlasResource() const
{
    return m_impl ? m_impl->atlas.Get() : nullptr;
}

u64 GraphPreviewRenderer::AtlasHandle() const
{
    return m_impl ? m_impl->d.srvHeap->GetGpuHandle(m_impl->srvAtlas).ptr : 0;
}

void GraphPreviewRenderer::TileUv(int tile, float& u0, float& v0, float& u1, float& v1)
{
    const int cx = tile % static_cast<int>(kAtlasCells), cy = tile / static_cast<int>(kAtlasCells);
    const float s = 1.0f / static_cast<float>(kAtlasCells);
    // 隣のタイルの滲みを避けて 0.5 テクセル内側
    const float e = 0.5f / static_cast<float>(kAtlasSize);
    u0 = cx * s + e; v0 = cy * s + e;
    u1 = (cx + 1) * s - e; v1 = (cy + 1) * s - e;
}

// ---------------------------------------------------------------------------------------------
bool GraphPreviewRenderer::RenderMain(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const std::string& instanceKey,
                                      const matgraph::PreviewSettings& settingsIn, u32 pixels, float timeSec)
{
    if (!m_valid || !cmd) return false;
    Impl& I = *m_impl;
    matgraph::PreviewSettings st = settingsIn;
    st.Clamp();
    pixels = std::min(kMainSize, std::max(128u, pixels));
    m_lastPixels = pixels;
    const bool flat = I.flat != nullptr;
    if (!flat) I.BakeEnvironments();   // 失敗しても描く（IBL なし）
    const int envIdx = static_cast<int>(st.env);
    const Impl::Env* env = (!flat && I.envReady) ? &I.envs[std::min(2, std::max(0, envIdx))] : nullptr;

    // ---- カメラ・ライト・行列 ----
    const XMFLOAT3 cam = {0.0f, 0.0f, -st.dist};
    const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&cam), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(30.0f), 1.0f, 0.1f, 50.0f);
    const XMMATRIX world = XMMatrixRotationRollPitchYaw(st.pitch, st.yaw, 0.0f);
    // ライト: yaw / pitch = 「ライトのある向き」（カメラ側 = -Z が正面）。シェーダーへは進む向き（逆）を渡す
    const float lp = st.lightPitch, ly = st.lightYaw;
    XMFLOAT3 toLight = {std::cos(lp) * std::sin(ly), std::sin(lp), -std::cos(lp) * std::cos(ly)};
    XMFLOAT3 lightTravel = {-toLight.x, -toLight.y, -toLight.z};
    XMFLOAT3 lightColor = {1, 1, 1};
    float ambient = 0.0f;
    if (flat)
    {
        lightTravel = I.flat->lightDir;
        lightColor = I.flat->lightColor;
        ambient = I.flat->ambient;
    }
    else if (env)
    {
        lightColor = {env->lightColor[0] * env->lightIntensity, env->lightColor[1] * env->lightIntensity, env->lightColor[2] * env->lightIntensity};
    }
    else
    {
        lightColor = {2.0f, 1.9f, 1.8f};
        ambient = 0.4f;
    }
    FrameConstants fc;
    I.FillFrame(fc, view, proj, cam, lightTravel, lightColor, ambient, env != nullptr, env, timeSec);
    std::memcpy(I.CbPtr(frameIndex, 0), &fc, sizeof(fc));

    // ---- 描画: 色（RGBA16F）+ 深度 ----
    {
        D3D12_RESOURCE_BARRIER b[2] = {
            Trans(m_color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            Trans(I.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
        cmd->ResourceBarrier(2, b);
    }
    const float clear[4] = {0, 0, 0, 0};
    cmd->ClearRenderTargetView(I.rtvColor, clear, 0, nullptr);
    cmd->ClearDepthStencilView(I.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(1, &I.rtvColor, FALSE, &I.dsv);
    D3D12_VIEWPORT vp = {0, 0, static_cast<float>(pixels), static_cast<float>(pixels), 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, static_cast<LONG>(pixels), static_cast<LONG>(pixels)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    bool drawn = false;
    GraphMaterialSystem::DrawParams dp{};
    if (I.d.graphs->ResolveInstance(instanceKey, cmd, frameIndex, false, dp) && dp.pso)
    {
        I.BindCommon(cmd, I.CbAddr(frameIndex, 0), env);
        Mesh* mesh = st.shape == matgraph::PreviewShape::Plane ? I.plane.get()
                   : st.shape == matgraph::PreviewShape::Cylinder ? I.cylinder.get()
                   : st.shape == matgraph::PreviewShape::Cube ? I.cube.get() : I.sphere.get();
        I.DrawMesh(cmd, *mesh, world, view * proj, dp);
        drawn = true;
    }
    {
        D3D12_RESOURCE_BARRIER b[3] = {
            Trans(m_color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Trans(I.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Trans(m_display.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        cmd->ResourceBarrier(3, b);
    }

    // ---- 表示: 背景 + ACES + ガンマ ----
    cmd->OMSetRenderTargets(1, &I.rtvDisplay, FALSE, nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    {
        ID3D12DescriptorHeap* heaps[] = {I.d.srvHeap->GetHeap()};
        cmd->SetDescriptorHeaps(1, heaps);   // 描画が無かった（BindCommon を通らなかった）ときも必要
    }
    cmd->SetGraphicsRootSignature(I.tmRS.Get());
    cmd->SetPipelineState(I.tmPso.Get());
    cmd->SetGraphicsRootDescriptorTable(0, I.d.srvHeap->GetGpuHandle(I.srvColorDepth));
    float tm[16] = {};
    if (flat)
    {
        tm[0] = tm[1] = tm[2] = 0.14f; tm[4] = tm[5] = tm[6] = 0.06f; tm[8] = 1.0f; tm[9] = 0.35f;
    }
    else
    {
        const Impl::Env& e = env ? *env : I.envs[0];
        tm[0] = e.topColor[0]; tm[1] = e.topColor[1]; tm[2] = e.topColor[2];
        tm[4] = e.botColor[0]; tm[5] = e.botColor[1]; tm[6] = e.botColor[2];
        tm[8] = e.exposure; tm[9] = e.vignette;
    }
    tm[11] = static_cast<float>(pixels);
    cmd->SetGraphicsRoot32BitConstants(1, 16, tm, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    {
        D3D12_RESOURCE_BARRIER b = Trans(m_display.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
    }
    ++m_mainRenders;
    return drawn;
}

// ---------------------------------------------------------------------------------------------
bool GraphPreviewRenderer::RequestCapture(ID3D12GraphicsCommandList* cmd, bool linear)
{
    if (!m_valid || !cmd || m_capture.active) return false;
    Impl& I = *m_impl;
    ID3D12Resource* src = linear ? m_color.Get() : m_display.Get();
    const u32 bpp = linear ? 8u : 4u;
    const u32 n = m_lastPixels;
    const u32 rowPitch = (n * bpp + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(rowPitch) * n;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(I.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;
    D3D12_RESOURCE_BARRIER b = Trans(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, sl{};
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = src->GetDesc().Format;
    dst.PlacedFootprint.Footprint.Width = n;
    dst.PlacedFootprint.Footprint.Height = n;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    sl.pResource = src;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = {0, 0, 0, n, n, 1};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &sl, &box);
    b = Trans(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
    m_capture.rb = rb;
    m_capture.w = m_capture.h = n;
    m_capture.bpp = bpp;
    m_capture.rowPitch = rowPitch;
    m_capture.issuedAt = m_frameCounter;
    m_capture.linear = linear;
    m_capture.active = true;
    return true;
}

bool GraphPreviewRenderer::PollCapture(std::vector<u8>& data, u32& w, u32& h, u32& bytesPerPixel, bool& linear)
{
    if (!m_capture.active || m_frameCounter < m_capture.issuedAt + 4) return false;   // フレーム多重化（3）+ 1 のあとなら GPU は終わっている
    u8* mapped = nullptr;
    if (FAILED(m_capture.rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) { m_capture = Capture{}; return false; }
    w = m_capture.w; h = m_capture.h; bytesPerPixel = m_capture.bpp; linear = m_capture.linear;
    data.resize(static_cast<size_t>(w) * h * bytesPerPixel);
    for (u32 y = 0; y < h; ++y)
        std::memcpy(&data[static_cast<size_t>(y) * w * bytesPerPixel], mapped + static_cast<size_t>(y) * m_capture.rowPitch, static_cast<size_t>(w) * bytesPerPixel);
    m_capture.rb->Unmap(0, nullptr);
    m_capture = Capture{};
    return true;
}

// ---------------------------------------------------------------------------------------------
void GraphPreviewRenderer::ClearTile(ID3D12GraphicsCommandList* cmd, int tile)
{
    if (!m_valid || tile < 0 || tile >= static_cast<int>(kAtlasCells * kAtlasCells)) return;
    Impl& I = *m_impl;
    D3D12_RESOURCE_BARRIER b = Trans(I.atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);
    const int cx = tile % static_cast<int>(kAtlasCells), cy = tile / static_cast<int>(kAtlasCells);
    const D3D12_RECT r = {cx * static_cast<LONG>(kTilePixels), cy * static_cast<LONG>(kTilePixels), (cx + 1) * static_cast<LONG>(kTilePixels), (cy + 1) * static_cast<LONG>(kTilePixels)};
    const float col[4] = {0.10f, 0.10f, 0.11f, 1.0f};
    cmd->ClearRenderTargetView(I.rtvAtlas, col, 1, &r);
    b = Trans(I.atlas.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
}

int GraphPreviewRenderer::RenderTiles(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const std::vector<TileRequest>& reqs, std::vector<int>* drawn)
{
    if (!m_valid || !cmd) return 0;
    Impl& I = *m_impl;
    // 描ける物を先に確かめる（Resolve は RS / RT の設定に依存しないので、バリアの前に）
    struct Item { int tile; GraphMaterialSystem::DrawParams dp; };
    std::vector<Item> items;
    for (const TileRequest& r : reqs)
    {
        if (r.tile < 0 || r.tile >= static_cast<int>(kAtlasCells * kAtlasCells)) continue;
        GraphMaterialSystem::DrawParams dp{};
        if (I.d.graphs->ResolveInstance(r.instanceKey, cmd, frameIndex, false, dp) && dp.pso) items.push_back({r.tile, dp});
    }
    if (items.empty() && I.atlasCleared) return 0;

    D3D12_RESOURCE_BARRIER b = Trans(I.atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);
    if (!I.atlasCleared)
    {
        const float col[4] = {0.10f, 0.10f, 0.11f, 1.0f};
        cmd->ClearRenderTargetView(I.rtvAtlas, col, 0, nullptr);
        I.atlasCleared = true;
    }
    if (!items.empty())
    {
        FrameConstants fc;
        I.FillFrame(fc, XMMatrixIdentity(), XMMatrixIdentity(), {0, 0, -1}, {0, -1, 0}, {0, 0, 0}, 0.0f, false, nullptr, 0.0f);
        std::memcpy(I.CbPtr(frameIndex, 1), &fc, sizeof(fc));
        cmd->OMSetRenderTargets(1, &I.rtvAtlas, FALSE, nullptr);
        I.BindCommon(cmd, I.CbAddr(frameIndex, 1), nullptr);
        for (const Item& it : items)
        {
            const int cx = it.tile % static_cast<int>(kAtlasCells), cy = it.tile / static_cast<int>(kAtlasCells);
            D3D12_VIEWPORT vp = {static_cast<float>(cx * kTilePixels), static_cast<float>(cy * kTilePixels), static_cast<float>(kTilePixels), static_cast<float>(kTilePixels), 0.0f, 1.0f};
            D3D12_RECT sc = {cx * static_cast<LONG>(kTilePixels), cy * static_cast<LONG>(kTilePixels), (cx + 1) * static_cast<LONG>(kTilePixels), (cy + 1) * static_cast<LONG>(kTilePixels)};
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &sc);
            I.DrawMesh(cmd, *I.quad, XMMatrixIdentity(), XMMatrixIdentity(), it.dp);
            ++m_tileRenders;
            if (drawn) drawn->push_back(it.tile);
        }
    }
    b = Trans(I.atlas.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
    return static_cast<int>(items.size());
}

bool GraphPreviewRenderer::RenderRawTile(ID3D12GraphicsCommandList* cmd, u32 frameIndex, const std::string& instanceKey)
{
    if (!m_valid || !cmd) return false;
    Impl& I = *m_impl;
    GraphMaterialSystem::DrawParams dp{};
    if (!I.d.graphs->ResolveInstance(instanceKey, cmd, frameIndex, false, dp) || !dp.pso) return false;
    D3D12_RESOURCE_BARRIER b = Trans(m_rawTile.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);
    const float clear[4] = {0, 0, 0, 0};
    cmd->ClearRenderTargetView(I.rtvRaw, clear, 0, nullptr);
    FrameConstants fc;
    I.FillFrame(fc, XMMatrixIdentity(), XMMatrixIdentity(), {0, 0, -1}, {0, -1, 0}, {0, 0, 0}, 0.0f, false, nullptr, 0.0f);
    std::memcpy(I.CbPtr(frameIndex, 1), &fc, sizeof(fc));
    cmd->OMSetRenderTargets(1, &I.rtvRaw, FALSE, nullptr);
    I.BindCommon(cmd, I.CbAddr(frameIndex, 1), nullptr);
    D3D12_VIEWPORT vp = {0, 0, static_cast<float>(kTilePixels), static_cast<float>(kTilePixels), 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, static_cast<LONG>(kTilePixels), static_cast<LONG>(kTilePixels)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    I.DrawMesh(cmd, *I.quad, XMMatrixIdentity(), XMMatrixIdentity(), dp);
    b = Trans(m_rawTile.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
    ++m_tileRenders;
    return true;
}

} // namespace dx12e
