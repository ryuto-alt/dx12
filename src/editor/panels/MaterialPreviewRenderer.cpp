#include "editor/panels/MaterialPreviewRenderer.h"

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandList.h"
#include "graphics/PipelineState.h"
#include "graphics/Texture.h"
#include "renderer/Mesh.h"
#include "resource/MaterialAssetIO.h"
#include "resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace DirectX;

namespace dx12e
{

namespace
{
// [0]PerObject(b0,32定数) / [1]PreviewLight(b1,24定数) / [2]SRVテーブルt0-t2(albedo/normal/metalRoughness)
// / [3]PBRMaterial(b2,4定数)。メインの9スロットRootSignature(src/graphics/RootSignature.cpp)とは
// 完全に別物(プレビュー専用、CSM/IBL/SSAOテーブルを一切持たない=バインド漏れの心配が無い)。
Microsoft::WRL::ComPtr<ID3D12RootSignature> BuildPreviewRootSignature(GraphicsDevice& device)
{
    D3D12_ROOT_PARAMETER1 rootParams[4]{};

    rootParams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.Num32BitValues = 32;
    rootParams[0].Constants.ShaderRegister = 0;  // b0
    rootParams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[1].Constants.Num32BitValues = 24;
    rootParams[1].Constants.ShaderRegister = 1;  // b1
    rootParams[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 3;  // t0,t1,t2 連続
    srvRange.BaseShaderRegister                = 0;
    srvRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[3].Constants.Num32BitValues = 4;
    rootParams[3].Constants.ShaderRegister = 2;  // b2
    rootParams[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;  // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1.NumParameters     = _countof(rootParams);
    versionedDesc.Desc_1_1.pParameters       = rootParams;
    versionedDesc.Desc_1_1.NumStaticSamplers = 1;
    versionedDesc.Desc_1_1.pStaticSamplers   = &sampler;
    versionedDesc.Desc_1_1.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &serialized, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            Logger::Error("マテリアルプレビューRootSignatureのシリアライズに失敗: {}",
                          static_cast<const char*>(errorBlob->GetBufferPointer()));
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
    hr = device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(&rs));
    if (FAILED(hr))
    {
        Logger::Error("マテリアルプレビューRootSignatureの作成に失敗しました");
        return nullptr;
    }
    return rs;
}

// PreviewLight(b1)と1:1のレイアウト(HLSL側 shaders/forward/MaterialPreview.hlsl と揃えること)。
struct PreviewLightParams
{
    XMFLOAT3 camPos;   f32 pad0;
    XMFLOAT3 keyDir;   f32 keyIntensity;
    XMFLOAT3 keyColor; f32 pad1;
    XMFLOAT3 fillDir;  f32 fillIntensity;
    XMFLOAT3 fillColor;f32 pad2;
    f32      ambient;  XMFLOAT3 pad3;
};

// サムネイルキャッシュのキー。directory_iterator由来(\\区切り)と assetsDir+rel 由来(/混在)の
// 両方から同じエントリへ届くよう、正規化して/区切りへ統一する。
std::string NormalizeThumbKey(const std::string& absPath)
{
    return std::filesystem::path(absPath).lexically_normal().generic_string();
}
} // namespace

void MaterialPreviewRenderer::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager)
{
    m_device = &device;
    m_srvHeap = srvHeap;
    m_resourceManager = resourceManager;

    m_previewRtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    constexpr float clearColor[4] = {0.05f, 0.05f, 0.06f, 1.0f};
    m_previewRT.Initialize(device, &m_previewRtvHeap, srvHeap, kPreviewSize, kPreviewSize,
                           DXGI_FORMAT_R8G8B8A8_UNORM, clearColor);

    BuildDepthBuffer(device);

    for (u32 i = 0; i < kSrvRingCount; ++i)
        m_srvBlocks[i] = srvHeap->AllocateBlock(3);

    // ---- 球体サムネイル用の共有リソース(RTVスロット1個 + 128px深度) ----
    m_thumbRtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    m_thumbRtvHandle = m_thumbRtvHeap.Allocate();
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = kThumbSize;
        desc.Height           = kThumbSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc       = {1, 0};
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format       = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        if (SUCCEEDED(device.GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &clearValue, IID_PPV_ARGS(&m_thumbDepth))))
        {
            m_thumbDsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
            m_thumbDsvHandle = m_thumbDsvHeap.Allocate();
            device.GetDevice()->CreateDepthStencilView(m_thumbDepth.Get(), nullptr, m_thumbDsvHandle);
        }
    }

    m_sphereMesh = std::make_unique<Mesh>();
    m_sphereMesh->InitializeAsSphere(device, 1.0f, 32, 32);
    m_planeMesh = std::make_unique<Mesh>();
    m_planeMesh->InitializeAsPlane(device, 2.0f, 4);

    if (!m_compiler.Initialize())
    {
        Logger::Warn("マテリアルプレビュー: シェーダーの実行時コンパイルが利用できません(3Dプレビュー無効)");
        return;
    }

    BuildPipeline(device);
}

void MaterialPreviewRenderer::BuildDepthBuffer(GraphicsDevice& device)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width             = kPreviewSize;
    desc.Height            = kPreviewSize;
    desc.DepthOrArraySize  = 1;
    desc.MipLevels         = 1;
    desc.Format            = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc        = {1, 0};
    desc.Flags             = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format               = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil         = {1.0f, 0};

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device.GetDevice()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue, IID_PPV_ARGS(&m_depthBuffer));
    if (FAILED(hr))
    {
        Logger::Warn("マテリアルプレビュー: デプスバッファの作成に失敗しました");
        return;
    }

    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    m_dsvHandle = m_dsvHeap.Allocate();
    device.GetDevice()->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsvHandle);
}

void MaterialPreviewRenderer::BuildPipeline(GraphicsDevice& device)
{
    const std::wstring hlslPath = PathResolver::ShaderSourceDirW() + L"forward/MaterialPreview.hlsl";
    const std::vector<std::wstring> includeDirs = { PathResolver::ShaderSourceDirW() + L"forward/" };

    ShaderRuntimeCompiler::CompileRequest vsReq;
    vsReq.hlslPath = hlslPath; vsReq.entry = L"VSMain"; vsReq.profile = L"vs_6_0"; vsReq.includeDirs = includeDirs;
    ShaderRuntimeCompiler::CompileResult vsResult = m_compiler.Compile(vsReq);

    ShaderRuntimeCompiler::CompileRequest psReq;
    psReq.hlslPath = hlslPath; psReq.entry = L"PSMain"; psReq.profile = L"ps_6_0"; psReq.includeDirs = includeDirs;
    ShaderRuntimeCompiler::CompileResult psResult = m_compiler.Compile(psReq);

    if (!vsResult.success || !psResult.success)
    {
        Logger::Error("マテリアルプレビューシェーダーのコンパイルに失敗しました: {} {}",
                      vsResult.errorLog, psResult.errorLog);
        return;
    }

    m_rootSignature = BuildPreviewRootSignature(device);
    if (!m_rootSignature)
        return;

    PipelineStateBuilder builder;
    builder.SetRootSignature(m_rootSignature.Get())
           .SetVertexShader(vsResult.dxil.data(), vsResult.dxil.size())
           .SetPixelShader(psResult.dxil.data(), psResult.dxil.size())
           .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
           .SetRenderTargetFormat(DXGI_FORMAT_R8G8B8A8_UNORM)
           .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
           .SetDepthEnabled(true)
           .SetCullMode(D3D12_CULL_MODE_NONE);

    m_pso = std::make_unique<PipelineState>();
    m_pso->Initialize(device, builder);

    m_valid = true;
    Logger::Info("マテリアルプレビュー: パイプライン初期化完了");
}

void MaterialPreviewRenderer::Render(CommandList& cmd, const DrawInput& input, MaterialPreviewShape shape,
                                     f32 camYaw, f32 camPitch, f32 camDist)
{
    if (!m_valid || !m_resourceManager) return;

    m_previewRT.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    DrawSceneTo(cmd, m_previewRT.GetRtv(), m_dsvHandle, kPreviewSize, input, shape, camYaw, camPitch, camDist);
    m_previewRT.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void MaterialPreviewRenderer::DrawSceneTo(CommandList& cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                          D3D12_CPU_DESCRIPTOR_HANDLE dsv, u32 size,
                                          const DrawInput& input, MaterialPreviewShape shape,
                                          f32 camYaw, f32 camPitch, f32 camDist)
{
    ID3D12GraphicsCommandList* nativeCmdList = cmd.GetNative();

    const u32 srvBlockStart = m_srvBlocks[m_srvRingCursor];
    m_srvRingCursor = (m_srvRingCursor + 1) % kSrvRingCount;
    // テクスチャ解決は Application::EnsureMaterialOverrideSrv/MaterialAssetManager と同じ方針:
    // albedo=sRGB、normal・metalRoughness(ARM)=linear。欠落分はResourceManagerのデフォルトへ。
    auto resolve = [&](const std::string& relPath, Texture* fallback, bool srgb) -> Texture* {
        if (relPath.empty() || !dx12e::vfs::Exists(relPath))
            return fallback;
        std::string fullPath = PathResolver::AssetsDir() + relPath;
        return m_resourceManager->GetOrLoadTexture(PathResolver::Utf8ToWide(fullPath), nativeCmdList, srgb);
    };

    const bool hasNormal = !input.normalPath.empty();
    const bool hasMetalRoughness = !input.metalRoughnessPath.empty();

    Texture* albedo = resolve(input.albedoPath, m_resourceManager->GetDefaultWhiteTexture(), /*srgb=*/true);
    Texture* normal = resolve(input.normalPath, m_resourceManager->GetDefaultNormalTexture(), /*srgb=*/false);
    Texture* mr     = resolve(input.metalRoughnessPath, m_resourceManager->GetDefaultMetalRoughnessTexture(), /*srgb=*/false);
    if (!albedo || !normal || !mr) return;

    albedo->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvBlockStart));
    normal->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvBlockStart + 1));
    mr->CreateSRV(*m_device, m_srvHeap->GetCpuHandle(srvBlockStart + 2));

    // オービットカメラ(球面座標→LookAt)。VfxEditorPanelと同じ方式。原点(マテリアルボール中心)を注視。
    const XMVECTOR target = XMVectorZero();
    const f32 cp = std::cos(camPitch), sp = std::sin(camPitch);
    const f32 cy = std::cos(camYaw),   sy = std::sin(camYaw);
    const XMVECTOR offset = XMVectorScale(XMVectorSet(cp * cy, sp, cp * sy, 0.0f), camDist);
    const XMVECTOR eye = XMVectorAdd(target, offset);
    const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(40.0f), 1.0f, 0.1f, 100.0f);
    const XMMATRIX viewProj = view * proj;
    XMFLOAT3 camPos; XMStoreFloat3(&camPos, eye);

    constexpr float clearColor[4] = {0.05f, 0.05f, 0.06f, 1.0f};
    cmd.ClearRenderTarget(rtv, clearColor);
    cmd.ClearDepthStencil(dsv, 1.0f);

    ID3D12GraphicsCommandList* native = cmd.GetNative();
    native->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cmd.SetViewportAndScissor(size, size);

    cmd.SetDescriptorHeap(m_srvHeap->GetHeap());
    native->SetGraphicsRootSignature(m_rootSignature.Get());
    native->SetPipelineState(m_pso->Get());

    struct { XMMATRIX mvp; XMMATRIX model; } objData;
    objData.mvp   = XMMatrixTranspose(viewProj);   // モデル行列は恒等(球/平面は原点生成済み)
    objData.model = XMMatrixTranspose(XMMatrixIdentity());
    native->SetGraphicsRoot32BitConstants(0, 32, &objData, 0);

    PreviewLightParams light{};
    light.camPos = camPos;
    light.keyDir = {0.4f, -0.75f, 0.5f}; light.keyIntensity = 3.2f; light.keyColor = {1.0f, 0.98f, 0.94f};
    light.fillDir = {-0.5f, -0.2f, -0.6f}; light.fillIntensity = 0.9f; light.fillColor = {0.55f, 0.65f, 0.85f};
    light.ambient = 0.18f;
    native->SetGraphicsRoot32BitConstants(1, 24, &light, 0);

    native->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGpuHandle(srvBlockStart));

    struct { f32 metallic; f32 roughness; u32 flags; f32 pad; } pbrParams;
    pbrParams.metallic  = input.metallic;
    pbrParams.roughness = input.roughness;
    pbrParams.flags     = (hasNormal ? 1u : 0u) | (hasMetalRoughness ? 2u : 0u);
    pbrParams.pad       = 0.0f;
    native->SetGraphicsRoot32BitConstants(3, 4, &pbrParams, 0);

    Mesh* mesh = (shape == MaterialPreviewShape::Sphere) ? m_sphereMesh.get() : m_planeMesh.get();
    cmd.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd.SetVertexBuffer(mesh->GetVertexBuffer().GetView());
    cmd.SetIndexBuffer(mesh->GetIndexBuffer().GetView());
    cmd.DrawIndexedInstanced(mesh->GetIndexCount());
}

u64 MaterialPreviewRenderer::GetOrQueueThumbnail(const std::string& dxmatAbsPath)
{
    if (!m_valid || !m_thumbDepth) return 0;

    const std::string key = NormalizeThumbKey(dxmatAbsPath);
    auto it = m_thumbCache.find(key);
    if (it != m_thumbCache.end())
        return it->second.failed ? 0 : it->second.gpuHandle;

    if (std::find(m_thumbQueue.begin(), m_thumbQueue.end(), key) == m_thumbQueue.end())
        m_thumbQueue.push_back(key);
    return 0;
}

void MaterialPreviewRenderer::InvalidateThumbnail(const std::string& dxmatAbsPath)
{
    const std::string key = NormalizeThumbKey(dxmatAbsPath);
    auto it = m_thumbCache.find(key);
    if (it != m_thumbCache.end() && it->second.failed)
    {
        // failed エントリはテクスチャを持たないので消して作り直させる
        m_thumbCache.erase(it);
        return;
    }
    if (it == m_thumbCache.end()) return;   // 未生成なら次の GetOrQueueThumbnail が積む

    // 既存テクスチャへ再レンダリング(解放しない)
    if (std::find(m_thumbQueue.begin(), m_thumbQueue.end(), key) == m_thumbQueue.end())
        m_thumbQueue.push_back(key);
}

size_t MaterialPreviewRenderer::ScanAllMaterials(const std::string& assetsDir)
{
    if (!m_valid || !m_thumbDepth) return 0;

    namespace fs = std::filesystem;
    size_t queued = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(assetsDir), fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec))
    {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".dxmat") continue;

        const std::string key = NormalizeThumbKey(it->path().string());
        if (m_thumbCache.count(key)) continue;
        if (std::find(m_thumbQueue.begin(), m_thumbQueue.end(), key) == m_thumbQueue.end())
        {
            m_thumbQueue.push_back(key);
            ++queued;
        }
    }
    return queued;
}

// キャッシュファイル名: キー(正規化絶対パス)のハッシュ。モデルサムネと同じ .thumbcache/ 内、
// 衝突ドメインを分けるため "m" プレフィックス付き。
std::string MaterialPreviewRenderer::ThumbCacheFilePath(const std::string& key) const
{
    const size_t h = std::hash<std::string>{}(key);
    char filename[40];
    snprintf(filename, sizeof(filename), "m%016zx.raw", h);
    return PathResolver::AssetsDir() + ".thumbcache/" + filename;
}

// ディスクキャッシュ(128x128 RGBA8 raw、rowPitch=512)からサムネイルを復元する。
// デコードも球描画も不要で 64KB のアップロードだけ=1件あたり<1ms。成功で true。
bool MaterialPreviewRenderer::LoadThumbFromCache(CommandList& cmd, const std::string& key,
                                                 const std::string& cacheFile)
{
    constexpr u32 kRowPitch = kThumbSize * 4;               // 512 (256アライン済み)
    constexpr u32 kDataSize = kRowPitch * kThumbSize;       // 65536

    std::vector<uint8_t> bytes;
    {
        std::ifstream ifs(std::filesystem::path(cacheFile), std::ios::binary);
        if (!ifs) return false;
        bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (bytes.size() != kDataSize) return false;        // 壊れたキャッシュ→再レンダリングへ
    }

    ID3D12Device* dev = m_device->GetDevice();
    ThumbEntry entry;

    // 本体テクスチャ(再レンダリング(Invalidate)にも使うのでRT可・初期状態COPY_DEST)
    {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = kThumbSize;
        texDesc.Height           = kThumbSize;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc       = {1, 0};
        texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal{};
        clearVal.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        clearVal.Color[0] = 0.05f; clearVal.Color[1] = 0.05f;
        clearVal.Color[2] = 0.06f; clearVal.Color[3] = 1.0f;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                &clearVal, IID_PPV_ARGS(&entry.texture))))
            return false;
    }

    // アップロードバッファ(GPU完了まで m_thumbUploads で生存させる)
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = kDataSize;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&upload))))
            return false;
        void* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
        std::memcpy(mapped, bytes.data(), kDataSize);
        upload->Unmap(0, nullptr);
    }

    ID3D12GraphicsCommandList* native = cmd.GetNative();
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kThumbSize, kThumbSize, 1, kRowPitch};
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = entry.texture.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        native->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = entry.texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        native->ResourceBarrier(1, &barrier);
    }
    m_thumbUploads.push_back({std::move(upload), m_thumbFrame});

    entry.srvIndex = m_srvHeap->AllocateIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(entry.texture.Get(), &srvDesc,
                                  m_srvHeap->GetCpuHandle(entry.srvIndex));
    entry.gpuHandle = m_srvHeap->GetGpuHandle(entry.srvIndex).ptr;

    m_thumbCache[key] = std::move(entry);
    return true;
}

void MaterialPreviewRenderer::RenderPendingThumbnails(CommandList& cmd)
{
    if (!m_valid || !m_resourceManager || !m_thumbDepth) return;

    ID3D12GraphicsCommandList* native = cmd.GetNative();
    ID3D12Device* dev = m_device->GetDevice();

    ++m_thumbFrame;

    // 1) レンダリング済みサムネイルのディスク保存(発行から4フレーム後=トリプルバッファでも
    //    GPU完了が保証されるタイミング。FrameResources::BeginFrame が過去フレームのフェンスを待つ)
    for (auto it = m_thumbSaves.begin(); it != m_thumbSaves.end();)
    {
        if (m_thumbFrame - it->frame >= 4)
        {
            void* mapped = nullptr;
            const D3D12_RANGE readRange{0, kThumbSize * kThumbSize * 4};
            if (SUCCEEDED(it->readback->Map(0, &readRange, &mapped)))
            {
                std::error_code ec;
                std::filesystem::create_directories(
                    std::filesystem::path(it->file).parent_path(), ec);
                std::ofstream ofs(std::filesystem::path(it->file), std::ios::binary | std::ios::trunc);
                if (ofs)
                    ofs.write(static_cast<const char*>(mapped),
                              static_cast<std::streamsize>(kThumbSize * kThumbSize * 4));
                const D3D12_RANGE writeRange{0, 0};
                it->readback->Unmap(0, &writeRange);
            }
            it = m_thumbSaves.erase(it);
        }
        else
            ++it;
    }
    // 2) 役目を終えたアップロードバッファの解放(同じく4フレーム後)
    std::erase_if(m_thumbUploads, [&](const ThumbPendingUpload& u) {
        return m_thumbFrame - u.frame >= 4;
    });

    // 3) キュー処理: ディスクキャッシュヒットは軽い(64KBアップロードのみ)ので16件/フレーム、
    //    レンダリング(テクスチャデコード込みで重い)は2件/フレームまで。
    int rendered = 0, cacheLoaded = 0;
    while (!m_thumbQueue.empty() && cacheLoaded < 16)
    {
        const std::string key = m_thumbQueue.front();

        // ディスクキャッシュ: キャッシュが .dxmat より新しければ復元(初回生成エントリのみ。
        // Invalidate 再レンダリングは既存エントリがあるのでキャッシュを使わず描き直す)
        const std::string cacheFile = ThumbCacheFilePath(key);
        if (!m_thumbCache.count(key))
        {
            std::error_code ec1, ec2;
            namespace fs = std::filesystem;
            if (fs::exists(fs::path(cacheFile), ec1)
                && fs::last_write_time(fs::path(cacheFile), ec1) >= fs::last_write_time(fs::path(key), ec2)
                && !ec1 && !ec2
                && LoadThumbFromCache(cmd, key, cacheFile))
            {
                m_thumbQueue.erase(m_thumbQueue.begin());
                ++cacheLoaded;
                continue;
            }
        }

        if (rendered >= 2) break;   // 重い方の予算は使い切った(FIFOを保って次フレームへ)
        ++rendered;
        m_thumbQueue.erase(m_thumbQueue.begin());

        // .dxmat 読み込み+パース
        MaterialAssetData data;
        bool parsed = false;
        {
            std::ifstream ifs(std::filesystem::path(key), std::ios::binary);
            if (ifs)
            {
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                                            std::istreambuf_iterator<char>());
                parsed = ParseMaterialAsset(bytes, data);
            }
        }

        auto it = m_thumbCache.find(key);
        if (!parsed)
        {
            // 既存サムネイルがあれば古い絵のまま残す(壊れた保存の途中かもしれない)。無ければ failed 登録。
            if (it == m_thumbCache.end())
                m_thumbCache[key].failed = true;
            continue;
        }

        const bool needTexture = (it == m_thumbCache.end()) || !it->second.texture;
        ThumbEntry& entry = m_thumbCache[key];
        entry.failed = false;

        if (needTexture)
        {
            D3D12_RESOURCE_DESC texDesc{};
            texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width            = kThumbSize;
            texDesc.Height           = kThumbSize;
            texDesc.DepthOrArraySize = 1;
            texDesc.MipLevels        = 1;
            texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            texDesc.SampleDesc       = {1, 0};
            texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
            clearVal.Color[0] = 0.05f; clearVal.Color[1] = 0.05f;
            clearVal.Color[2] = 0.06f; clearVal.Color[3] = 1.0f;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            if (FAILED(dev->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    &clearVal, IID_PPV_ARGS(&entry.texture))))
            {
                entry.failed = true;
                continue;
            }
        }
        else
        {
            // 再レンダリング: PIXEL_SHADER_RESOURCE → RENDER_TARGET
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = entry.texture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            native->ResourceBarrier(1, &barrier);
        }

        // RTVは共有スロット1個を使い回す(記録時点で参照が焼き込まれるため上書きしてよい)
        dev->CreateRenderTargetView(entry.texture.Get(), nullptr, m_thumbRtvHandle);

        DrawInput input;
        input.albedoPath         = data.albedoPath;
        input.normalPath         = data.normalPath;
        input.metalRoughnessPath = data.metalRoughnessPath;
        input.metallic           = data.metallic;
        input.roughness          = data.roughness;
        // マテリアルエディタの初期カメラと同じ構図の球体で固定
        DrawSceneTo(cmd, m_thumbRtvHandle, m_thumbDsvHandle, kThumbSize,
                    input, MaterialPreviewShape::Sphere, 0.6f, 0.3f, 3.0f);

        // RT → COPY_SOURCE: 結果をリードバックへコピーしてディスクキャッシュに保存する
        // (保存自体はGPU完了が確実な4フレーム後にMap。次回ロードからデコード不要になる)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = entry.texture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            native->ResourceBarrier(1, &barrier);
        }
        {
            constexpr u32 kRowPitch = kThumbSize * 4;
            constexpr u32 kDataSize = kRowPitch * kThumbSize;
            Microsoft::WRL::ComPtr<ID3D12Resource> readback;
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = kDataSize;
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.Format           = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc       = {1, 0};
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(dev->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&readback))))
            {
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource        = entry.texture.Get();
                src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = readback.Get();
                dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint =
                    {DXGI_FORMAT_R8G8B8A8_UNORM, kThumbSize, kThumbSize, 1, kRowPitch};
                native->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                m_thumbSaves.push_back({ThumbCacheFilePath(key), std::move(readback), m_thumbFrame});
            }
        }
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = entry.texture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            native->ResourceBarrier(1, &barrier);
        }

        if (entry.srvIndex == 0xFFFFFFFFu)
        {
            entry.srvIndex = m_srvHeap->AllocateIndex();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels     = 1;
            dev->CreateShaderResourceView(entry.texture.Get(), &srvDesc,
                                          m_srvHeap->GetCpuHandle(entry.srvIndex));
            entry.gpuHandle = m_srvHeap->GetGpuHandle(entry.srvIndex).ptr;
        }
    }
}

u64 MaterialPreviewRenderer::GetPreviewGpuHandle() const
{
    if (!m_valid || !m_srvHeap) return 0;
    return m_srvHeap->GetGpuHandle(m_previewRT.GetSrvIndex()).ptr;
}

} // namespace dx12e
