#include "editor/ModelThumbnailRenderer.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RootSignature.h"
#include "graphics/PipelineState.h"
#include "graphics/Texture.h"
#include "resource/ResourceManager.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "core/Logger.h"
#include "core/Assert.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace dx12e
{

void ModelThumbnailRenderer::Initialize(GraphicsDevice* device,
                                         DescriptorHeap* srvHeap,
                                         ResourceManager* resourceManager,
                                         RootSignature* rootSignature,
                                         PipelineState* pipelineState)
{
    m_device      = device;
    m_srvHeap     = srvHeap;
    m_resourceMgr = resourceManager;
    m_rootSig     = rootSignature;
    m_pso         = pipelineState;

    CreateSharedResources();
}

void ModelThumbnailRenderer::CreateSharedResources()
{
    auto* dev = m_device->GetDevice();

    // ===== 共有デプスバッファ =====
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = kThumbSize;
        desc.Height           = kThumbSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearVal{};
        clearVal.Format               = DXGI_FORMAT_D32_FLOAT;
        clearVal.DepthStencil.Depth   = 1.0f;
        clearVal.DepthStencil.Stencil = 0;

        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        ThrowIfFailed(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal,
            IID_PPV_ARGS(&m_depthBuffer)));
    }

    // ===== RTV ヒープ (1 descriptor) =====
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    // ===== DSV ヒープ (1 descriptor) =====
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = 1;
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_dsvHeap)));
        m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_dsvHandle);
    }

    // ===== PerFrame アップロードバッファ (256B) =====
    {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = 256;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_perFrameUpload)));
    }
}

void ModelThumbnailRenderer::Request(const std::string& modelPath)
{
    if (m_cache.count(modelPath)) return;
    // 重複チェック
    for (const auto& p : m_pendingQueue)
        if (p == modelPath) return;
    m_pendingQueue.push_back(modelPath);
}

u64 ModelThumbnailRenderer::GetCachedHandle(const std::string& modelPath) const
{
    auto it = m_cache.find(modelPath);
    if (it != m_cache.end()) return it->second.gpuHandle;
    return 0;
}

size_t ModelThumbnailRenderer::ScanAllModels(const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    m_pendingQueue.clear();

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj")
            m_pendingQueue.push_back(entry.path().string());
    }

    m_totalScanned = m_pendingQueue.size();
    Logger::Info("[Thumbnail] Found {} models to render", m_totalScanned);
    return m_totalScanned;
}

size_t ModelThumbnailRenderer::RenderNext(ID3D12GraphicsCommandList* cmdList)
{
    if (m_pendingQueue.empty()) return 0;
    std::string path = std::move(m_pendingQueue.back());
    m_pendingQueue.pop_back();
    RenderOne(path, cmdList);
    return m_pendingQueue.size();
}

void ModelThumbnailRenderer::RenderPending(ID3D12GraphicsCommandList* cmdList, u32 /*frameIndex*/)
{
    if (m_pendingQueue.empty()) return;

    std::string modelPath = std::move(m_pendingQueue.back());
    m_pendingQueue.pop_back();
    RenderOne(modelPath, cmdList);
}

void ModelThumbnailRenderer::RenderOne(const std::string& modelPath,
                                        ID3D12GraphicsCommandList* cmdList)
{
    if (m_cache.count(modelPath)) return;

    const CachedModel* model = m_resourceMgr->GetOrLoadModel(modelPath, cmdList);
    if (!model || model->meshes.empty())
    {
        // ロード失敗 → 空のキャッシュエントリ
        m_cache[modelPath] = ThumbEntry{};
        return;
    }

    auto* dev = m_device->GetDevice();

    // ===== サムネイルテクスチャ作成 =====
    ThumbEntry entry;
    {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = kThumbSize;
        texDesc.Height           = kThumbSize;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal{};
        clearVal.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        clearVal.Color[0] = 0.15f;
        clearVal.Color[1] = 0.15f;
        clearVal.Color[2] = 0.18f;
        clearVal.Color[3] = 1.0f;

        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        ThrowIfFailed(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&entry.texture)));
    }

    // RTV 作成（共有ヒープの1つ目を再利用）
    dev->CreateRenderTargetView(entry.texture.Get(), nullptr, m_rtvHandle);

    // ===== AABB 計算 → カメラ配置 =====
    XMFLOAT3 aabbMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    XMFLOAT3 aabbMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const auto& mesh : model->meshes)
    {
        auto mn = mesh->GetAABBMin();
        auto mx = mesh->GetAABBMax();
        aabbMin.x = (std::min)(aabbMin.x, mn.x);
        aabbMin.y = (std::min)(aabbMin.y, mn.y);
        aabbMin.z = (std::min)(aabbMin.z, mn.z);
        aabbMax.x = (std::max)(aabbMax.x, mx.x);
        aabbMax.y = (std::max)(aabbMax.y, mx.y);
        aabbMax.z = (std::max)(aabbMax.z, mx.z);
    }

    XMFLOAT3 center = {
        (aabbMin.x + aabbMax.x) * 0.5f,
        (aabbMin.y + aabbMax.y) * 0.5f,
        (aabbMin.z + aabbMax.z) * 0.5f
    };
    float extX = aabbMax.x - aabbMin.x;
    float extY = aabbMax.y - aabbMin.y;
    float extZ = aabbMax.z - aabbMin.z;
    float maxExtent = (std::max)({extX, extY, extZ});
    if (maxExtent < 0.001f) maxExtent = 1.0f;

    // カメラを斜め上から見下ろす位置に配置
    float dist = maxExtent * 1.3f;
    XMFLOAT3 camPos = {
        center.x + dist * 0.6f,
        center.y + dist * 0.4f,
        center.z + dist * 0.6f
    };

    XMMATRIX viewMat = XMMatrixLookAtLH(
        XMLoadFloat3(&camPos),
        XMLoadFloat3(&center),
        XMVectorSet(0, 1, 0, 0));
    XMMATRIX projMat = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, 1.0f, maxExtent * 0.01f, maxExtent * 10.0f);

    // ===== PerFrame CB 書き込み =====
    struct FrameConstants {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT3   lightDir;
        float      time;
        XMFLOAT3   lightColor;
        float      ambientStrength;
        XMFLOAT4X4 lightViewProj;
        XMFLOAT3   cameraPos;
        float      _pad;
    };
    {
        FrameConstants fc{};
        XMStoreFloat4x4(&fc.view, XMMatrixTranspose(viewMat));
        XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(projMat));
        XMVECTOR ld = XMVector3Normalize(XMVectorSet(-0.5f, -1.0f, -0.3f, 0));
        XMStoreFloat3(&fc.lightDir, ld);
        fc.time = 0;
        fc.lightColor = {1.0f, 0.95f, 0.9f};
        fc.ambientStrength = 0.35f;
        XMStoreFloat4x4(&fc.lightViewProj, XMMatrixIdentity());
        fc.cameraPos = camPos;

        void* mapped = nullptr;
        m_perFrameUpload->Map(0, nullptr, &mapped);
        std::memcpy(mapped, &fc, sizeof(fc));
        m_perFrameUpload->Unmap(0, nullptr);
    }

    // ===== 描画 =====
    // クリア
    float clearColor[4] = {0.15f, 0.15f, 0.18f, 1.0f};
    cmdList->ClearRenderTargetView(m_rtvHandle, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(m_dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // レンダーターゲット設定
    cmdList->OMSetRenderTargets(1, &m_rtvHandle, FALSE, &m_dsvHandle);

    // ビューポート / シザー
    D3D12_VIEWPORT vp = {0, 0, static_cast<float>(kThumbSize), static_cast<float>(kThumbSize), 0, 1};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(kThumbSize), static_cast<LONG>(kThumbSize)};
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &scissor);

    // パイプライン設定
    cmdList->SetGraphicsRootSignature(m_rootSig->Get());
    cmdList->SetPipelineState(m_pso->Get());

    // SRV ヒープ
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // PerFrame CBV
    cmdList->SetGraphicsRootConstantBufferView(
        RootSignature::kSlotPerFrame,
        m_perFrameUpload->GetGPUVirtualAddress());

    // Shadow SRV (ダミー — デフォルト白テクスチャ)
    Texture* defTex = m_resourceMgr->GetDefaultWhiteTexture();
    if (defTex)
    {
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotShadowSRV,
            m_srvHeap->GetGpuHandle(defTex->GetSrvIndex()));
    }

    // 各メッシュを描画
    XMMATRIX worldMat = XMMatrixIdentity();
    XMMATRIX viewProj = viewMat * projMat;

    for (const auto& mesh : model->meshes)
    {
        // PerObject (MVP + Model)
        struct { XMMATRIX mvp; XMMATRIX mdl; } objData;
        objData.mvp = XMMatrixTranspose(worldMat * viewProj);
        objData.mdl = XMMatrixTranspose(worldMat);
        cmdList->SetGraphicsRoot32BitConstants(
            RootSignature::kSlotPerObject, 32, &objData, 0);

        // テクスチャ
        const Material* mat = mesh->GetMaterial();
        if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
        {
            cmdList->SetGraphicsRootDescriptorTable(
                RootSignature::kSlotSRVTable,
                m_srvHeap->GetGpuHandle(mat->srvBlockIndex));
        }
        else
        {
            Texture* wt = m_resourceMgr->GetDefaultWhiteTexture();
            cmdList->SetGraphicsRootDescriptorTable(
                RootSignature::kSlotSRVTable,
                m_srvHeap->GetGpuHandle(wt->GetSrvIndex()));
        }

        // PBR
        struct { float metallic; float roughness; u32 flags; float pad; } pbr;
        pbr.metallic  = mat ? mat->defaultMetallic  : 0.0f;
        pbr.roughness = mat ? mat->defaultRoughness : 0.5f;
        pbr.flags     = 0;
        pbr.pad       = 0;
        cmdList->SetGraphicsRoot32BitConstants(
            RootSignature::kSlotPBRMaterial, 4, &pbr, 0);

        // 頂点/インデックス
        auto& vbv = mesh->GetVertexBuffer().GetView();
        auto& ibv = mesh->GetIndexBuffer().GetView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawIndexedInstanced(mesh->GetIndexCount(), 1, 0, 0, 0);
    }

    // ===== RT → SRV に遷移 =====
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = entry.texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // SRV 作成
    entry.srvIndex = m_srvHeap->AllocateIndex();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        dev->CreateShaderResourceView(
            entry.texture.Get(), &srvDesc,
            m_srvHeap->GetCpuHandle(entry.srvIndex));
    }
    entry.gpuHandle = m_srvHeap->GetGpuHandle(entry.srvIndex).ptr;

    m_cache[modelPath] = std::move(entry);
    Logger::Info("[Thumbnail] Rendered: {}", modelPath);
}

} // namespace dx12e
