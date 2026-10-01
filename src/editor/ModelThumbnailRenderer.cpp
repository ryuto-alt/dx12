#include "editor/ModelThumbnailRenderer.h"
#include "editor/AssetBrowserLogic.h"
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
}

void ModelThumbnailRenderer::SetCacheRoot(const std::string& assetsDir)
{
    std::string dir = assetsDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
    dir += ".thumbcache/";
    if (dir == m_cacheDir) return;
    m_cacheDir = dir;
    // 別プロジェクトへ切り替えた: 常駐は全部墓場へ（GPU が使い終わるまで遅延解放）、未処理の要求は捨てる。
    for (auto& kv : m_cache)
        if (kv.second.gpu.Valid()) m_graveyard.push_back({std::move(kv.second.gpu), m_frame});
    m_cache.clear();
    m_queue.clear();
    m_queued.clear();
    m_diskMiss.clear();
    m_resident = 0;
}

void ModelThumbnailRenderer::BeginFrameRequests()
{
    m_queue.clear();
    m_queued.clear();
}

void ModelThumbnailRenderer::Request(const std::string& modelPath, int64_t mtime, uint64_t size)
{
    auto it = m_cache.find(modelPath);
    if (it != m_cache.end() && it->second.mtime == mtime && it->second.size == size) return;   // 描画済み（または読めなかった）
    if (!m_queued.insert(modelPath).second) return;
    m_queue.push_back(Req{modelPath, mtime, size});
}

u64 ModelThumbnailRenderer::GetCachedHandle(const std::string& modelPath) const
{
    auto it = m_cache.find(modelPath);
    if (it == m_cache.end()) return 0;
    it->second.lastUsed = m_frame;
    return it->second.gpu.Valid() ? it->second.gpu.handle : 0;
}

std::string ModelThumbnailRenderer::CacheFilePath(const std::string& modelPath) const
{
    return m_cacheDir + abl::ThumbFileName(modelPath);
}

void ModelThumbnailRenderer::RenderPending(ID3D12GraphicsCommandList* cmdList, u32 /*frameIndex*/)
{
    ++m_frame;
    if (!m_device || !cmdList) return;

    // ---- 遅延解放（GPU が使い終わった物）----
    constexpr u64 kGraveFrames = 8;
    while (!m_graveyard.empty() && m_frame - m_graveyard.front().frame >= kGraveFrames)
    {
        if (m_graveyard.front().gpu.srv != 0xFFFFFFFFu) m_srvHeap->Free(m_graveyard.front().gpu.srv);
        m_graveyard.pop_front();
    }
    while (!m_uploadKeep.empty() && m_frame - m_uploadKeep.front().first >= kGraveFrames)
        m_uploadKeep.pop_front();

    // ---- 描画したサムネイルのリードバック → ディスク（GPU が終わった数フレーム後。WaitIdle しない）----
    for (size_t i = 0; i < m_saves.size();)
    {
        PendingSave& s = m_saves[i];
        if (m_frame - s.frame < 6) { ++i; continue; }
        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, kThumbDataSize};
        if (SUCCEEDED(s.readback->Map(0, &readRange, &mapped)))
        {
            abl::WriteThumbFile(std::filesystem::path(CacheFilePath(s.path)), kThumbSize, kThumbSize, s.mtime, s.size,
                                static_cast<const uint8_t*>(mapped));
            D3D12_RANGE writeRange = {0, 0};
            s.readback->Unmap(0, &writeRange);
        }
        m_saves.erase(m_saves.begin() + static_cast<ptrdiff_t>(i));
    }

    // ---- 今フレームの要求を処理（ディスクキャッシュは数枚、描画は 1 枚まで。残りは次フレームに再要求される）----
    int diskLoads = 0, renders = 0;
    for (const Req& req : m_queue)
    {
        if (diskLoads >= kDiskLoadsPerFrame && renders >= kRendersPerFrame) break;
        // 先にディスクキャッシュ（安い）。無かった物は覚えて、毎フレーム開き直さない。
        if (diskLoads < kDiskLoadsPerFrame)
        {
            auto miss = m_diskMiss.find(req.path);
            const bool knownMiss = miss != m_diskMiss.end() && miss->second.first == req.mtime && miss->second.second == req.size;
            uint32_t w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (!knownMiss)
            {
                if (abl::ReadThumbFile(std::filesystem::path(CacheFilePath(req.path)), req.mtime, req.size, w, h, rgba)
                    && w == kThumbSize && h == kThumbSize)
                {
                    ThumbGpu g;
                    if (CreateThumbTexture(*m_device, *m_srvHeap, cmdList, rgba.data(), w, h, g))
                    {
                        ThumbEntry& e = m_cache[req.path];
                        if (e.gpu.Valid()) m_graveyard.push_back({std::move(e.gpu), m_frame}); else ++m_resident;
                        m_uploadKeep.emplace_back(m_frame, g.upload);
                        e.gpu = std::move(g);
                        e.gpu.upload.Reset();
                        e.mtime = req.mtime; e.size = req.size; e.failed = false; e.lastUsed = m_frame;
                        ++diskLoads;
                        ++m_diskLoadedTotal;
                        continue;
                    }
                }
                m_diskMiss[req.path] = {req.mtime, req.size};
            }
        }
        if (renders < kRendersPerFrame)
        {
            RenderOne(req, cmdList);
            ++renders;
        }
    }
    m_queue.clear();
    m_queued.clear();

    // ---- 常駐数の上限 ----
    if (m_resident > kMaxResident)
    {
        std::vector<std::pair<u64, std::string>> order;
        for (auto& kv : m_cache)
            if (kv.second.gpu.Valid() && m_frame - kv.second.lastUsed > 120) order.emplace_back(kv.second.lastUsed, kv.first);
        std::sort(order.begin(), order.end());
        const size_t excess = m_resident - kMaxResident + kMaxResident / 8;
        for (size_t i = 0; i < order.size() && i < excess; ++i)
        {
            ThumbEntry& e = m_cache[order[i].second];
            m_graveyard.push_back({std::move(e.gpu), m_frame});
            m_cache.erase(order[i].second);
            --m_resident;
        }
    }
}

bool ModelThumbnailRenderer::RenderOne(const Req& req, ID3D12GraphicsCommandList* cmdList)
{
    const std::string& modelPath = req.path;
    const CachedModel* model = m_resourceMgr->GetOrLoadModel(modelPath, cmdList);
    if (!model || model->meshes.empty())
    {
        // ロード失敗 → 空のキャッシュエントリ（元ファイルが変わるまで再試行しない）
        ThumbEntry& e = m_cache[modelPath];
        e.mtime = req.mtime; e.size = req.size; e.failed = true;
        return false;
    }

    auto* dev = m_device->GetDevice();

    // ===== サムネイルテクスチャ作成 =====
    ThumbGpu gpu;
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
            IID_PPV_ARGS(&gpu.tex)));
    }

    // RTV 作成（共有ヒープの1つ目を再利用）
    dev->CreateRenderTargetView(gpu.tex.Get(), nullptr, m_rtvHandle);

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
        XMFLOAT4   pcssParams;                            // 16B  (offset 544) PCSS（サムネイルでは常に 0＝従来 PCF）
        // ▼ DDGI 48B (offset 560)。サムネイルでは常に 0＝t22 を読まない
        XMFLOAT4   ddgiOrigin;                            // 16B  (offset 560)
        XMFLOAT4   ddgiSpacing;                           // 16B  (offset 576)
        XMFLOAT4   ddgiCounts;                            // 16B  (offset 592)
        XMFLOAT4   _clusterReserved[35];                  // 560B (offset 608..1167)
        XMFLOAT4   ddgiC1;                                // 16B  (offset 1168) GI S4。サムネでは 0
        XMFLOAT4   ddgiScroll0;                           // 16B  (offset 1184)
        XMFLOAT4   ddgiScroll1;                           // 16B  (offset 1200)
        XMFLOAT4   giParams;                              // 16B  (offset 1216) GI モード。サムネでは 0＝Legacy
        XMFLOAT4   giParams2;                             // 16B  (offset 1232)
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
        // ★環境光を強めに。サムネイルには IBL も影もライトも無く、ambient だけが陰の側を持ち上げる。
        //   0.35 だと金属寄りのマテリアルや暗い albedo が「真っ黒なシルエット」になり、何のモデルか読めなかった。
        fc.ambientStrength = 0.55f;

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
        // ★clusterExtra.w = デカール数（計画06）。サムネイルは 0 = デカール無効。
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

    // SRV ヒープ
    // ★メイン RS は CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED（バインドレス）を立てていることがある
    //   （RootSignature::IsBindless。マテリアルグラフ G2a）。その RS を Set する【前】に
    //   SetDescriptorHeaps を呼ぶ規約。逆順だとデバッグレイヤが id=1419 を出す。
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // パイプライン設定
    cmdList->SetGraphicsRootSignature(m_rootSig->Get());
    cmdList->SetPipelineState(m_pso->Get());

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
        struct { float metallic; float roughness; u32 flags; u32 packedTint;
                 float uvScaleX, uvScaleY, uvOffsetX, uvOffsetY; u32 packedEmissive; } pbr;
        // ★金属度は上限を掛ける。glTF は metallicFactor 既定 1.0 なので、環境（IBL）の無いサムネイルでは
        //   拡散反射が 0 になって「真っ黒」に見えた（金属は環境の映り込みでしか見えない）。
        pbr.metallic  = mat ? (std::min)(mat->defaultMetallic, 0.3f) : 0.0f;
        pbr.roughness = mat ? (std::max)(mat->defaultRoughness, 0.35f) : 0.5f;
        pbr.flags     = 0;
        // ★4 番目は packedTint（RGB888 の一律ティント + 上位 8bit が不透明度。Forward.hlsl の b2）。
        //   以前ここを 0 で埋めていた＝ティントが「黒」・不透明度 0 になり、**全モデルのサムネイルが真っ黒の透けたシルエット**になっていた
        //   （ImGui は alpha 0 の画素を透過して描くので、パネルの地色が見えるだけ）。0xFFFFFFFF = 白・不透明 = 影響なし。
        pbr.packedTint = 0xFFFFFFFFu;
        // サムネイルは UV スクロール/連番を適用しない（恒等変換）
        pbr.uvScaleX = 1.0f; pbr.uvScaleY = 1.0f;
        pbr.uvOffsetX = 0.0f; pbr.uvOffsetY = 0.0f;
        // 自己発光。★モデル側に SRV ブロックがあるときだけテクスチャを許す（無いときは
        //   白 1 枚だけを貼るフォールバックで 4 枚目が別物になるため）。
        if (mat && mat->srvBlockIndex != 0xFFFFFFFF && mat->emissiveTexture)
            pbr.flags |= kPbrFlagEmissiveTex;
        pbr.packedEmissive = mat
            ? PackEmissive(ResolveEmissiveParams(mat->emissiveColor, mat->emissiveIntensity,
                                                 DirectX::XMFLOAT3{-1.0f, -1.0f, -1.0f}, -1.0f))
            : 0u;
        cmdList->SetGraphicsRoot32BitConstants(
            RootSignature::kSlotPBRMaterial, 9, &pbr, 0);

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
        barrier.Transition.pResource   = gpu.tex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // リードバックバッファへコピー（ディスクキャッシュ保存用。数フレーム後に PendingSave が書き出す）
    {
        PendingSave ps;
        ps.path = modelPath; ps.mtime = req.mtime; ps.size = req.size; ps.frame = m_frame;
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
        if (SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ps.readback))))
        {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource        = gpu.tex.Get();
            src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = ps.readback.Get();
            dst.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset             = 0;
            dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width    = kThumbSize;
            dst.PlacedFootprint.Footprint.Height   = kThumbSize;
            dst.PlacedFootprint.Footprint.Depth    = 1;
            dst.PlacedFootprint.Footprint.RowPitch = kThumbRowPitch;

            cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            m_saves.push_back(std::move(ps));
        }
    }

    // ===== COPY_SOURCE → SRV =====
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = gpu.tex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // SRV 作成
    gpu.srv = m_srvHeap->AllocateIndex();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        dev->CreateShaderResourceView(
            gpu.tex.Get(), &srvDesc,
            m_srvHeap->GetCpuHandle(gpu.srv));
    }
    gpu.handle = m_srvHeap->GetGpuHandle(gpu.srv).ptr;
    gpu.width = kThumbSize; gpu.height = kThumbSize;

    ThumbEntry& e = m_cache[modelPath];
    if (e.gpu.Valid()) m_graveyard.push_back({std::move(e.gpu), m_frame}); else ++m_resident;
    e.gpu = std::move(gpu);
    e.mtime = req.mtime; e.size = req.size; e.failed = false; e.lastUsed = m_frame;
    ++m_renderedTotal;
    Logger::Info("[Thumbnail] Rendered: {}", modelPath);
    return true;
}

} // namespace dx12e
