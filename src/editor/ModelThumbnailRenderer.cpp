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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

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

    // ===== PerFrame アップロードバッファ =====
    // main の Forward PSO を流用するため b1 のレイアウトは Application.cpp の FrameConstants(1536B)
    // と一致させる。256B アラインで 1536B 確保（CSM の cascadeViewProj[4]/cascadeSplitsView/
    // shadowParams/スポット影行列/IBL/コンタクトシャドウまで含めて書けるサイズ）。
    {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = 1536;  // >= 1520、256B アライン
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

    // ===== Readback バッファ（キャッシュ保存用） =====
    {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = kThumbDataSize;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_readbackBuffer)));
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

std::string ModelThumbnailRenderer::GetCacheFilePath(const std::string& modelPath) const
{
    size_t h = std::hash<std::string>{}(modelPath);
    char filename[32];
    snprintf(filename, sizeof(filename), "%016zx.raw", h);
    return m_cacheDir + filename;
}

size_t ModelThumbnailRenderer::ScanAllModels(const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    m_pendingQueue.clear();
    m_cachedPaths.clear();

    // キャッシュディレクトリ設定
    m_cacheDir = assetsDir + ".thumbcache/";
    fs::create_directories(m_cacheDir);

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(assetsDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".gltf" && ext != ".glb" && ext != ".fbx" && ext != ".obj")
            continue;

        std::string modelPath = entry.path().string();
        std::string cachePath = GetCacheFilePath(modelPath);

        // キャッシュが存在し、モデルファイルより新しければスキップ
        if (fs::exists(cachePath))
        {
            auto cacheTime = fs::last_write_time(cachePath);
            auto modelTime = fs::last_write_time(entry.path());
            if (cacheTime >= modelTime)
            {
                m_cachedPaths.push_back(modelPath);
                continue;
            }
        }

        m_pendingQueue.push_back(modelPath);
    }

    m_totalScanned = m_pendingQueue.size() + m_cachedPaths.size();
    Logger::Info("[Thumbnail] {} models: {} cached, {} to render",
        m_totalScanned, m_cachedPaths.size(), m_pendingQueue.size());
    return m_pendingQueue.size(); // レンダリングが必要な数を返す
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
    // main の Forward PSO(Forward.hlsl)を流用するので b1 のレイアウトは
    // Application.cpp の FrameConstants(1536B) / Lighting.hlsli の PerFrameConstants と
    // バイト単位で一致させること。サムネには影を出さないため CSM 領域は
    // 「cascade0 を必ず選ばせて UV クリップ → CalcShadow が 1.0(無影) を返す」値に倒す。
    // スポット/ポイント影も numPointLights/numSpotLights=0 で未使用（shadowIndex 参照自体が発生しない）。
    static constexpr u32 kMaxShadowSpotThumb = 4;  // = MAX_SHADOW_SPOT (Lighting.hlsli)
    struct FrameConstants {
        XMFLOAT4X4 view;                          // 64B  (offset   0)
        XMFLOAT4X4 proj;                          // 64B  (offset  64)
        XMFLOAT3   lightDir;        float time;   // 16B  (offset 128)
        XMFLOAT3   lightColor;      float ambientStrength; // 16B (offset 144)
        XMFLOAT4X4 cascadeViewProj[4];            // 256B (offset 160)
        XMFLOAT4   cascadeSplitsView;             // 16B  (offset 416)
        XMFLOAT4   shadowParams;                  // 16B  (offset 432)
        XMFLOAT3   cameraPos;       float _pad;   // 16B  (offset 448)
        u32        numPointLights;  u32 numSpotLights;
        float      spotShadowTexel; float pointShadowNear;   // 16B (offset 464)
        // ▼ クラスタードライティング 64B (offset 480)。旧 pointLights[8]/spotLights[8] の跡地。
        // サムネイルは灯数 0 なので clusterGrid.w=0（総当たりフォールバック）で十分。
        XMFLOAT4   clusterParams;    // (offset 480)
        XMFLOAT4   clusterGrid;      // (offset 496)
        XMFLOAT4   clusterViewport;  // (offset 512)
        XMFLOAT4   clusterExtra;     // (offset 528)
        XMFLOAT4   _clusterReserved[44];                  // 704B (offset 544..1247)
        XMFLOAT4X4 spotShadowMatrix[kMaxShadowSpotThumb]; // 256B (offset 1248)
        // ▼ IBL 制御 16B (offset 1504)
        float iblIntensity;
        float maxPrefilterMip;
        u32   hasIBL;
        float skyboxIntensity;
        // ▼ コンタクトシャドウ制御 16B (offset 1520)。サムネは白ダミー(t11)なので 0 固定。
        float contactShadowEnabled;
        XMFLOAT3 _csPad;
    };  // total = 1536B
    static_assert(sizeof(FrameConstants) == 1536, "FrameConstants must be 1536 bytes (match Application.cpp / Lighting.hlsli)");
    {
        FrameConstants fc{};
        XMStoreFloat4x4(&fc.view, XMMatrixTranspose(viewMat));
        XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(projMat));
        XMVECTOR ld = XMVector3Normalize(XMVectorSet(-0.5f, -1.0f, -0.3f, 0));
        XMStoreFloat3(&fc.lightDir, ld);
        fc.time = 0;
        fc.lightColor = {1.0f, 0.95f, 0.9f};
        fc.ambientStrength = 0.35f;

        // CSM 無影化: cascade0=identity(残りも identity)。cascadeSplitsView は全成分を
        // 巨大正値にして SelectCascade が必ず cascade0 を返すようにする。identity 変換だと
        // worldPos がほぼそのまま UV になり大半が [0,1] 外 → SampleCascade が 1.0(無影) を返す。
        // 万一クリップ内に入っても shadowParams.x=1/size で破綻せず、band=0/debug=0 のため無害。
        XMMATRIX id = XMMatrixIdentity();
        for (int i = 0; i < 4; ++i)
            XMStoreFloat4x4(&fc.cascadeViewProj[i], id);
        fc.cascadeSplitsView = {1e9f, 1e9f, 1e9f, 1e9f};
        fc.shadowParams = {1.0f / 4096.0f, 0.0f, 0.0f, 0.0f}; // x=1/size, y=bias, z=band(0), w=debug(0)

        fc.cameraPos = camPos;

        // ライト/IBL なし: numPointLights/numSpotLights=0、hasIBL=0(ambient フォールバック)。
        fc.numPointLights  = 0;
        fc.numSpotLights   = 0;
        // クラスタードは無効（clusterGrid.w=0）+ 灯数 0 なので PS のライトループは 0 周。
        // clusterParams は log2 に食わせないので 0 のままで安全。
        fc.clusterGrid  = {16.0f, 9.0f, 24.0f, 0.0f};
        fc.clusterExtra = {0.0f, 128.0f, 0.0f, 0.0f};
        fc.iblIntensity    = 0.0f;
        fc.maxPrefilterMip = 4.0f;
        fc.hasIBL          = 0u;
        fc.skyboxIntensity = 0.0f;

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
        // スポット/ポイント影SRV(t9,t10)も同様にダミーで埋める（サムネイルは numPoint/SpotLights=0
        // なのでシェーダ側では実際に読まれない。未セットのままだとルートシグネチャ検証に引っかかるため）。
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotPunctualShadowSRV,
            m_srvHeap->GetGpuHandle(defTex->GetSrvIndex()));
    }

    // SSAO AO SRV (t8) / コンタクトシャドウ SRV (t11): forward PS が無条件で Load するので
    // 白ダミー(R8_UNORM=1.0)を必ずバインドする（どちらも同じ 1x1 白を共用）。
    if (m_aoWhiteSrvIndex != 0xFFFFFFFFu)
    {
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotAOSRV,
            m_srvHeap->GetGpuHandle(m_aoWhiteSrvIndex));
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotContactShadowSRV,
            m_srvHeap->GetGpuHandle(m_aoWhiteSrvIndex));
    }

    // SSR SRV (t16) / SSGI SRV (t17): forward PS が無条件で Load するので黒ダミーを必ず張る。
    // サムネイルはメインカメラと別視点なので、本物の SSR/SSGI を渡してはいけない。
    if (m_ssBlackSrvIndex != 0xFFFFFFFFu)
    {
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotSsrSRV,
            m_srvHeap->GetGpuHandle(m_ssBlackSrvIndex));
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotSsgiSRV,
            m_srvHeap->GetGpuHandle(m_ssBlackSrvIndex));
    }

    // クラスタライトテーブル (t13,t14,t15 + デカール予約 t18..t21)。
    // 灯数 0 + clusterGrid.w=0 なので実際には読まれないが、テーブルは必ずバインドする。
    if (m_clusterSrvIndex != 0xFFFFFFFFu)
    {
        cmdList->SetGraphicsRootDescriptorTable(
            RootSignature::kSlotClusterSRV,
            m_srvHeap->GetGpuHandle(m_clusterSrvIndex));
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
        struct { float metallic; float roughness; u32 flags; float pad;
                 float uvScaleX, uvScaleY, uvOffsetX, uvOffsetY; } pbr;
        pbr.metallic  = mat ? mat->defaultMetallic  : 0.0f;
        pbr.roughness = mat ? mat->defaultRoughness : 0.5f;
        pbr.flags     = 0;
        pbr.pad       = 0;
        // サムネイルは UV スクロール/連番を適用しない（恒等変換）
        pbr.uvScaleX = 1.0f; pbr.uvScaleY = 1.0f;
        pbr.uvOffsetX = 0.0f; pbr.uvOffsetY = 0.0f;
        cmdList->SetGraphicsRoot32BitConstants(
            RootSignature::kSlotPBRMaterial, 8, &pbr, 0);

        // 頂点/インデックス
        auto& vbv = mesh->GetVertexBuffer().GetView();
        auto& ibv = mesh->GetIndexBuffer().GetView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawIndexedInstanced(mesh->GetIndexCount(), 1, 0, 0, 0);
    }

    // ===== RT → COPY_SOURCE（リードバック用） =====
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = entry.texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // リードバックバッファへコピー（キャッシュ保存用）
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = entry.texture.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = m_readbackBuffer.Get();
        dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset             = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = kThumbSize;
        dst.PlacedFootprint.Footprint.Height   = kThumbSize;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kThumbRowPitch;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    m_lastRenderedPath = modelPath;

    // ===== COPY_SOURCE → SRV =====
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = entry.texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
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

void ModelThumbnailRenderer::SavePendingCache()
{
    if (m_lastRenderedPath.empty()) return;

    std::string cachePath = GetCacheFilePath(m_lastRenderedPath);

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, kThumbDataSize};
    if (SUCCEEDED(m_readbackBuffer->Map(0, &readRange, &mapped)))
    {
        std::ofstream ofs(cachePath, std::ios::binary);
        if (ofs.is_open())
            ofs.write(static_cast<const char*>(mapped), kThumbDataSize);
        D3D12_RANGE writeRange = {0, 0};
        m_readbackBuffer->Unmap(0, &writeRange);
    }

    m_lastRenderedPath.clear();
}

void ModelThumbnailRenderer::LoadCachedThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    if (m_cachedPaths.empty()) return;

    auto* dev = m_device->GetDevice();
    m_uploadBuffers.clear();

    for (const auto& modelPath : m_cachedPaths)
    {
        if (m_cache.count(modelPath)) continue;

        std::string cachePath = GetCacheFilePath(modelPath);
        std::ifstream ifs(cachePath, std::ios::binary);
        if (!ifs.is_open()) continue;

        std::vector<char> data(kThumbDataSize);
        ifs.read(data.data(), kThumbDataSize);
        if (ifs.gcount() != kThumbDataSize) continue;

        // GPU テクスチャ作成
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
            texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            ThrowIfFailed(dev->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &texDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&entry.texture)));
        }

        // アップロードバッファ
        ComPtr<ID3D12Resource> uploadBuf;
        {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = kThumbDataSize;
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.Format           = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadBuf)));
        }

        // アップロードバッファにデータ書き込み
        {
            void* mapped = nullptr;
            ThrowIfFailed(uploadBuf->Map(0, nullptr, &mapped));
            std::memcpy(mapped, data.data(), kThumbDataSize);
            uploadBuf->Unmap(0, nullptr);
        }

        // コピーコマンド: upload → texture
        {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = uploadBuf.Get();
            src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset             = 0;
            src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
            src.PlacedFootprint.Footprint.Width    = kThumbSize;
            src.PlacedFootprint.Footprint.Height   = kThumbSize;
            src.PlacedFootprint.Footprint.Depth    = 1;
            src.PlacedFootprint.Footprint.RowPitch = kThumbRowPitch;

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource        = entry.texture.Get();
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        // COPY_DEST → SRV
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = entry.texture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
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
        m_uploadBuffers.push_back(std::move(uploadBuf));
    }

    Logger::Info("[Thumbnail] Loaded {} cached thumbnails from disk", m_cachedPaths.size());
    m_cachedPaths.clear();
}

} // namespace dx12e
